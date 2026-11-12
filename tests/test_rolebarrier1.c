/*
 * Offline tests of the pure part of the role / barrier / restart module (native/rolebarrier1.c): no game, no Windows API, no network. Memory is a fake with the REAL bytes of
 * build 13298487 (taken from rerev.exe with a disassembler), the two PCs of the barrier are two rb_t objects joined by a simulated reliable line (delay, duplicates, reordering)
 * and a simulated session that follows the rules of coop_session.c (LOAD_BEGIN joins or advances the epoch, the host opens the barrier when everybody is loaded).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/coop_proto.h"
#include "../native/rolebarrier1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* a tiny fake memory for rb_table_triple (address -> value; unknown addresses read 0) */
static struct { uint32_t a, v; } tm[256];
static int tm_n;
static void tm_put(uint32_t a, uint32_t v)
{
    for (int i = 0; i < tm_n; i++)
        if (tm[i].a == a) {
            tm[i].v = v;
            return;
        }
    if (tm_n < 256) tm[tm_n].a = a, tm[tm_n].v = v, tm_n++;
}
static uint32_t tm_rd(void *user, uint32_t a)
{
    (void)user;
    for (int i = 0; i < tm_n; i++)
        if (tm[i].a == a) return tm[i].v;
    return 0;
}

static int wp_obj(void *user, uint32_t v)   /* the fake weapons of the bag test: 0x51000..0x54000 (uWp*), 0x55000 = uWpUnarmed and 0x60000 do not count */
{
    (void)user;
    return v >= 0x51000u && v <= 0x54000u;
}

static char logbuf[200000];
static void blogf(const char *line)
{
    size_t n = strlen(logbuf);
    if (n + strlen(line) + 2 < sizeof logbuf) snprintf(logbuf + n, sizeof logbuf - n, "%s\n", line);
}
static void mem_log(void *u, const char *line) { (void)u; blogf(line); }

/* ---------------------------------------------------------------- fake process memory */
typedef struct { uint32_t base, size; uint8_t *data; } region;
static region regions[16];
static int nregions;
static uint32_t heap_next = 0x70000000u;
static int fail_alloc, fail_write, fail_read;

static uint8_t *loc(uint32_t addr, uint32_t n)
{
    for (int i = 0; i < nregions; i++)
        if (addr >= regions[i].base && addr + n <= regions[i].base + regions[i].size) return regions[i].data + (addr - regions[i].base);
    return NULL;
}
static int m_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    uint8_t *p = fail_read ? NULL : loc(a, n);
    if (!p) return 0;
    memcpy(out, p, n);
    return 1;
}
static int m_write(void *u, uint32_t a, const void *src, uint32_t n)
{
    (void)u;
    uint8_t *p = fail_write ? NULL : loc(a, n);
    if (!p) return 0;
    memcpy(p, src, n);
    return 1;
}
static uint32_t m_alloc(void *u, uint32_t n)
{
    (void)u;
    if (fail_alloc) return 0;
    uint32_t a = heap_next;
    heap_next += (n + 15) & ~15u;
    regions[nregions].base = a, regions[nregions].size = n, regions[nregions].data = (uint8_t *)calloc(1, n);
    nregions++;
    return a;
}
static const rbk_mem MEM = {m_read, m_write, m_alloc, mem_log, NULL};

static void mem_reset(void)
{
    for (int i = 0; i < nregions; i++) free(regions[i].data);
    nregions = 0, heap_next = 0x70000000u, fail_alloc = fail_write = fail_read = 0;
    for (int i = 0; i < RBK_N; i++) {   /* each function: its real first bytes then 0xCC */
        regions[nregions].base = rbk_defs[i].va, regions[nregions].size = 64;
        regions[nregions].data = (uint8_t *)calloc(1, 64);
        memset(regions[nregions].data, 0xCC, 64);
        memcpy(regions[nregions].data, rbk_defs[i].sig, (size_t)rbk_defs[i].steal);
        nregions++;
    }
}

/* ---------------------------------------------------------------- the simulated session and line */
#define MAXQ 64
typedef struct { uint32_t due; uint8_t body[RB_MSG_MAX]; unsigned len; int to; } pkt;
static uint32_t T;                                   /* the clock of both PCs */
static pkt q[MAXQ];
static int nq;
static uint32_t line_delay = 20;                     /* ms */
static int line_dup, line_reorder;                   /* duplicate every packet / delay alternate packets more */
static int sent_count;

typedef struct {
    int started, phase;
    unsigned epoch;
    int me_load_epoch[2], loaded[2], gos[2], connected[2];
    uint32_t load_area[2];            /* rev1 live 8: the area each PC named in its last LOAD_BEGIN (CP_LOAD_BEGIN area_id) and that epoch */
    unsigned load_area_epoch[2];
} simsess;
static simsess S;

typedef struct { int id; rb_t b; int sends_ok; unsigned loads_begin_calls, loads_done_calls; } pc;
static pc P[2];

static void deliver_due(void)
{
    for (int i = 0; i < nq;) {
        if ((int32_t)(T - q[i].due) >= 0) {
            rb_on_event(&P[q[i].to].b, q[i].body, q[i].len);
            memmove(&q[i], &q[i + 1], (size_t)(nq - i - 1) * sizeof q[0]);
            nq--;
        } else
            i++;
    }
}

static uint32_t io_now(void *u) { (void)u; return T; }
static int io_session(void *u, rb_sinfo *o)
{
    pc *p = (pc *)u;
    (void)p;
    if (!S.started) return 0;
    o->started = 1, o->phase = S.phase, o->epoch = S.epoch, o->gos = S.gos[p->id];
    o->other_area = S.load_area[1 - p->id], o->other_area_epoch = S.load_area_epoch[1 - p->id];
    return 1;
}
static int io_send(void *u, const void *body, unsigned len)
{
    pc *p = (pc *)u;
    if (!p->sends_ok || !S.started || nq + 2 >= MAXQ) return 0;
    sent_count++;
    for (int c = 0; c < 1 + (line_dup != 0); c++) {
        pkt *k = &q[nq++];
        k->due = T + line_delay + ((line_reorder && (sent_count & 1)) ? 45u : 0u) + (uint32_t)c * 7u;
        memcpy(k->body, body, len), k->len = len, k->to = 1 - p->id;
    }
    return 1;
}
/* coop_session.c rules */
static void sim_check_open(void)
{
    if (S.phase == 2 && (!S.connected[0] || S.loaded[0]) && (!S.connected[1] || S.loaded[1])) {
        S.phase = 3;
        S.gos[0]++, S.gos[1]++;
    }
}
static void io_begin(void *u, uint32_t key)
{
    pc *p = (pc *)u;
    p->loads_begin_calls++;
    int fresh = !(S.phase == 2 && S.epoch != (unsigned)S.me_load_epoch[p->id]);
    if (fresh) {
        S.epoch++, S.phase = 2;
        S.loaded[0] = S.loaded[1] = 0;
    }
    S.me_load_epoch[p->id] = (int)S.epoch;
    S.loaded[p->id] = 0;
    S.load_area[p->id] = key, S.load_area_epoch[p->id] = S.epoch;
}
static void io_done(void *u)
{
    pc *p = (pc *)u;
    p->loads_done_calls++;
    if (S.phase == 2) {
        S.loaded[p->id] = 1;
        sim_check_open();
    }
}
static void io_log(void *u, const char *line) { (void)u; blogf(line); }

static void sim_start(void)
{
    memset(&S, 0, sizeof S);
    S.started = 1, S.phase = 2, S.epoch = 1, S.connected[0] = S.connected[1] = 1;   /* cs_start: LOADING, epoch 1 */
    nq = 0, sent_count = 0, T = 100000;
    line_delay = 20, line_dup = line_reorder = 0;
    for (int i = 0; i < 2; i++) {
        rb_io io = {io_now, io_session, io_send, io_begin, io_done, io_log, &P[i]};
        memset(&P[i], 0, sizeof P[i]);
        P[i].id = i, P[i].sends_ok = 1;
        rb_init(&P[i].b, &io, NULL);
        P[i].b.enabled = 1;
        P[i].b.host = i == 0;
    }
}
static void tick(int ms)
{
    for (int i = 0; i < ms; i++) {
        T++;
        deliver_due();
    }
}


/* ---------------------------------------------------------------- e1w2 (2026-11-08): the glue's "playable" as the live log showed it
 * After FUN_00720870 the game makes the player units within ~0.4 s and keeps them at 0,0,0 for the whole load / start movie (1-1: ~110 s, 1-2: ~3.5 min).
 * pred_old = the glue before the fix (an own unit exists), pred_new = an own unit that is placed (rb_placed). */
typedef struct { int unit; float pos[3]; } fake_pl;
static int pred_old(const fake_pl *f) { return f->unit; }
static int pred_new(const fake_pl *f) { return f->unit && rb_placed(f->pos[0], f->pos[1], f->pos[2]); }
static void fpl_set(fake_pl *f, int unit, float x, float y, float z) { f->unit = unit, f->pos[0] = x, f->pos[1] = y, f->pos[2] = z; }
/* the first load of a session: the title (no units) for 3 s, the host's restart call, the units at 0,0,0, the start movie, the host placed at movie_ms, the client
 * 300 ms later. Returns the ms (after the restart) at which the barrier opened, -1 = never within 130 s. */
static int e1w2_first_load(int (*pred)(const fake_pl *), int movie_ms)
{
    sim_start();
    fake_pl f[2];
    memset(f, 0, sizeof f);
    for (int i = 0; i < 3000; i++) { tick(1); rb_load_step(&P[0].b, pred(&f[0])); rb_load_step(&P[1].b, pred(&f[1])); }
    if (S.phase != 2) return -2;
    for (int i = 0; i < 130000; i++) {
        tick(1);
        for (int k = 0; k < 2; k++) {
            int placed_at = movie_ms + (k == 1 ? 300 : 0);
            if (i >= placed_at) fpl_set(&f[k], 1, -300.0f + 50.0f * (float)k, -200.0f, 18100.0f);
            else if (i >= 400) fpl_set(&f[k], 1, 0, 0, 0);
            rb_load_step(&P[k].b, pred(&f[k]));
        }
        if (S.phase == 3) return i;
    }
    return -1;
}
/* an in-session restart (1-1 -> 1-2 in e1w2): both placed and PLAYING; the units vanish for 370 ms, come back at 0,0,0 for the load (host 200 s, client 180 s),
 * then placed. *epoch_at = the ms of the new epoch (-1 none), *open_at = the ms the barrier opened again (-1 none). */
static void e1w2_restart(int (*pred)(const fake_pl *), int *epoch_at, int *open_at, int *early_open)
{
    sim_start();
    fake_pl f[2];
    fpl_set(&f[0], 1, -300, -200, 18100), fpl_set(&f[1], 1, -250, -200, 18160);
    for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, pred(&f[0])); rb_load_step(&P[1].b, pred(&f[1])); }
    *epoch_at = *open_at = -1, *early_open = 0;
    if (S.phase != 3 || S.epoch != 1) { *early_open = -1; return; }
    for (int i = 0; i < 230000; i++) {
        tick(1);
        for (int k = 0; k < 2; k++) {
            int placed_at = k == 0 ? 200000 : 180000;
            if (i < 370) fpl_set(&f[k], 0, 0, 0, 0);
            else if (i < placed_at) fpl_set(&f[k], 1, 0, 0, 0);
            else fpl_set(&f[k], 1, 2216.0f - 160.0f * (float)k, 380.0f, -1825.0f);
            rb_load_step(&P[k].b, pred(&f[k]));
        }
        if (*epoch_at < 0 && S.epoch == 2) *epoch_at = i;
        if (*epoch_at >= 0 && S.phase == 3 && *open_at < 0) *open_at = i;
        if (*epoch_at >= 0 && S.phase == 3 && i < 200000) *early_open = 1;
    }
}

/* ---------------------------------------------------------------- rev1 live 15: the guest's own save read before its follow at the title
 * A fake engine: the guest's level start gives its own human the bag of sItemBox, which holds the SAVED bag (3) only after this PC read its save; a level
 * loaded without the read gives the boot state (0 = m14d / m14h / m14j LIVE: `+30=uWpUnarmed +3c=uWpScanner`). kind_new = the host chose New Game (the guest
 * must keep the New Game set: no read). read_ms < 0 = the read never ends, refuse = the read cannot be asked. Returns the bag the guest's human got (-1 = no
 * call within 30 s); *trip = the triple of the guest's FUN_00720870, *asked = reads asked. */
static int tl_run(int title_load, int kind_new, int read_ms, int refuse, rb_triple *trip, int *asked)
{
    sim_start();
    P[1].b.cfg.title_load = title_load;
    rb_triple h13 = {0x91010300u, 256, 2};
    P[0].b.tx_diff = 1;
    P[0].b.tx_cp.have = 1, P[0].b.tx_cp.id = -1, P[0].b.tx_cp.mask = 0;
    if (kind_new) {
        rb_triple ng = {0x91010100u, 256, 1};
        h13 = ng;
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1 && rb_start_triple(&P[0].b, &h13) == RB_ST_SENT);   /* the title menu's New Game (FUN_00720540 from 0x83E896) */
    } else
        CHECK(rb_restart_host(&P[0].b, &h13) == 1);   /* the host's title Continue: its restart hook (FUN_00720870 from 0x6D1212) */
    tick(30);
    int saved = 0, bag = -1, read_on = 0;
    uint32_t read_t = 0;
    *asked = 0;
    for (int f = 0; f < 30000 / 16 && bag < 0; f++) {
        rb_triple fol, out;
        int follow = rb_restart_follow_at(&P[1].b, &fol, 1) == RB_FOL_CALL;
        if (follow) {
            int g = rb_title_load_gate(&P[1].b, &fol, 1);
            if (g == RB_TL_LOAD) {
                follow = 0, (*asked)++;
                rb_title_load_asked(&P[1].b, !refuse, refuse ? "no title menu manager" : NULL);
                if (refuse) follow = 1;
                else read_on = 1, read_t = T;
            } else if (g == RB_TL_WAIT)
                follow = 0;
        } else if (P[1].b.tl_state == RB_TLS_WAIT) {
            int done = read_on && read_ms >= 0 && (uint32_t)(T - read_t) >= (uint32_t)read_ms;
            if (done) saved = 1, read_on = 0;
            if (rb_title_load_frame(&P[1].b, 1, done, done, &out)) follow = 1, fol = out;
        }
        if (follow) {
            *trip = fol;
            bag = kind_new ? 7 : saved ? 3 : 0;   /* 7 = the New Game set (the level's, not the save's) */
            break;
        }
        tick(16);
    }
    P[0].b.tx_cp.have = 0, P[0].b.tx_diff = -1;
    return bag;
}

int main(void)
{
    /* ============================================ 0. numbers shared with other files */
    CHECK(CP_EV_RB_KIND == CP_EV_USER + 0x41);
    CHECK(CP_EV_RB_KIND != CP_EV_W1);
    CHECK(CP_PHASE_LOADING == 2 && CP_PHASE_PLAYING == 3);
    CHECK(CP_EVENT_MAX >= RB_MSG_MAX);
    for (int i = 0; i < RBK_N; i++) {
        CHECK(rbk_defs[i].steal >= 5 && rbk_defs[i].steal <= RBK_SIG_MAX);
        CHECK(rbk_defs[i].name && rbk_defs[i].what && rbk_defs[i].va > 0x400000u && rbk_defs[i].va < 0x1000000u);
    }
    /* the bytes of the build (rerev.exe 1573b79e..., read with a disassembler) */
    {
        static const uint8_t e[6] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C}, p[6] = {0x83, 0xEC, 0x08, 0x57, 0x8B, 0xF9}, mv[6] = {0x83, 0xEC, 0x54, 0x53, 0x33, 0xDB}, g[6] = {0x8B, 0x41, 0x28, 0x83, 0xF8, 0x10};
        static const uint8_t r[8] = {0x51, 0x56, 0x8B, 0xF1, 0x89, 0x74, 0x24, 0x04};
        CHECK(rbk_defs[RBK_EPEND].va == 0x42D160u && !memcmp(rbk_defs[RBK_EPEND].sig, e, 6) && rbk_defs[RBK_EPEND].steal == 6 && rbk_defs[RBK_EPEND].hold_ret == 4);
        CHECK(rbk_defs[RBK_PCS].va == 0x8916E0u && !memcmp(rbk_defs[RBK_PCS].sig, p, 6) && rbk_defs[RBK_PCS].hold_ret == -1);
        CHECK(rbk_defs[RBK_MOVIE].va == 0x785580u && !memcmp(rbk_defs[RBK_MOVIE].sig, mv, 6) && rbk_defs[RBK_MOVIE].hold_ret == -1);
        CHECK(rbk_defs[RBK_PCSEND].va == 0x88FC20u && !memcmp(rbk_defs[RBK_PCSEND].sig, g, 6) && rbk_defs[RBK_PCSEND].kind == RBK_POST);
        CHECK(rbk_defs[RBK_RESTART].va == 0x720870u && !memcmp(rbk_defs[RBK_RESTART].sig, r, 8) && rbk_defs[RBK_RESTART].steal == 8 && rbk_defs[RBK_RESTART].hold_ret == 0xC);
    }
    CHECK(sizeof(rbk_frame) == 4 * 11 + 4 * 8);
    CHECK((size_t)&((rbk_frame *)0)->hold_cell == 36 && (size_t)&((rbk_frame *)0)->arg == 44);   /* the stub writes [esp+24h] and the arguments follow the return address */

    /* ============================================ 1. role */
    {
        rb_role_t r;
        rb_role_act a;
        rb_role_init(&r);
        CHECK(rb_role_step(&r, 0, 1, &a) == RBR_NONE && !r.applied);                 /* not started */
        CHECK(rb_role_step(&r, 1, -1, &a) == RBR_NONE && !r.applied);                /* role unknown */
        CHECK(rb_role_step(&r, 1, 2, &a) == RBR_NONE);                               /* nonsense */
        CHECK(rb_role_step(&r, 1, 1, &a) == RBR_APPLY && a.role == 1 && a.own_req == 1 && r.applied && r.applies == 1 && a.prev == -1);
        CHECK(rb_role_step(&r, 1, 1, &a) == RBR_NONE && r.applies == 1);             /* once */
        CHECK(rb_role_step(&r, 1, 0, &a) == RBR_APPLY && a.role == 0 && a.own_req == -1 && r.applies == 2);   /* role changed: applied again */
        CHECK(a.prev == 1);   /* rev1 live 16: the co-op save's roles put back at the host's title Continue: the old role (its capture) is named, the glue unbinds it first */
        CHECK(rb_role_step(&r, 1, 0, &a) == RBR_NONE);
        CHECK(rb_role_step(&r, 0, 0, &a) == RBR_RELEASE && a.role == 0 && !r.applied && r.releases == 1);   /* session ended */
        CHECK(rb_role_step(&r, 0, 0, &a) == RBR_NONE && r.releases == 1);
        CHECK(rb_role_step(&r, 1, 1, &a) == RBR_APPLY && a.own_req == 1);            /* the next session */
        CHECK(rb_role_step(&r, 1, -1, &a) == RBR_RELEASE && a.role == 1);            /* the role became unknown (lost) */
        CHECK(rb_role_own_req(0) == -1 && rb_role_own_req(1) == 1 && rb_role_own_req(-1) == -1);
    }

    /* ============================================ 2. hooks on fake memory */
    {
        mem_reset();
        for (int i = 0; i < RBK_N; i++) {
            rbk_state s;
            memset(&s, 0, sizeof s);
            const rbk_def *d = &rbk_defs[i];
            CHECK(rbk_apply(&s, d, &MEM, 0x00ABCDEFu) == 1 && s.st == RBK_ON && s.block >= 0x70000000u && s.applies == 1);
            uint8_t *code = loc(d->va, (uint32_t)d->steal);
            uint8_t jmp[5];
            rbk_jmp_build(jmp, d->va, s.block);
            CHECK(code && !memcmp(code, jmp, 5));
            for (int k = 5; k < d->steal; k++) CHECK(code[k] == 0x90);
            CHECK(code[d->steal] == 0xCC);                          /* nothing after the stolen bytes touched */
            /* the block: the stolen bytes and a jump back to va + steal exist in it */
            uint8_t *blk = loc(s.block, 8);
            CHECK(blk != NULL);
            uint8_t *whole = loc(s.block, 100);
            int found_sig = 0, found_back = 0;
            for (int k = 0; whole && k < 100 - d->steal - 5; k++)
                if (!memcmp(whole + k, d->sig, (size_t)d->steal)) {
                    found_sig = 1;
                    uint8_t back[5];
                    rbk_jmp_build(back, s.block + (uint32_t)k + (uint32_t)d->steal, d->va + (uint32_t)d->steal);
                    found_back = !memcmp(whole + k + d->steal, back, 5);
                }
            CHECK(found_sig && found_back);
            CHECK(rbk_apply(&s, d, &MEM, 0x00ABCDEFu) == 1 && s.applies == 1);   /* again: nothing changes */
            char pr[80];
            CHECK(rbk_probe(d, &MEM, s.block, pr, sizeof pr) == 2 && !strcmp(pr, "detoured (ours)"));
            CHECK(rbk_revert(&s, d, &MEM) == 1 && s.st == RBK_OFF && !memcmp(loc(d->va, (uint32_t)d->steal), d->sig, (size_t)d->steal) && s.reverts == 1);
            CHECK(rbk_probe(d, &MEM, s.block, pr, sizeof pr) == 1 && !strcmp(pr, "original"));
            CHECK(rbk_revert(&s, d, &MEM) == 1 && s.reverts == 1);   /* revert twice is harmless */
            CHECK(rbk_apply(&s, d, &MEM, 0x00ABCDEFu) == 1 && s.applies == 2);   /* and the stub block is reused */
        }
        /* refusals: nothing is written, the reason says what is there */
        for (int i = 0; i < RBK_N; i++) {
            mem_reset();
            rbk_state s;
            memset(&s, 0, sizeof s);
            const rbk_def *d = &rbk_defs[i];
            uint8_t *code = loc(d->va, 1);
            code[2] ^= 0x40;   /* another build */
            uint8_t before[16];
            memcpy(before, loc(d->va, 16), 16);
            logbuf[0] = 0;
            CHECK(rbk_apply(&s, d, &MEM, 1) == 0 && s.st == RBK_MISMATCH && s.refusals == 1 && s.block == 0);
            CHECK(!memcmp(before, loc(d->va, 16), 16));
            CHECK(strstr(s.why, "expected the original build") != NULL);
            CHECK(strstr(logbuf, d->name) != NULL && strstr(logbuf, "refused") != NULL);
            char pr[80];
            CHECK(rbk_probe(d, &MEM, 0, pr, sizeof pr) == 0 && !strncmp(pr, "FOREIGN ", 8));
        }
        {   /* somebody else's detour (E9 ...) */
            mem_reset();
            rbk_state s;
            memset(&s, 0, sizeof s);
            uint8_t *code = loc(0x42D160u, 6);
            code[0] = 0xE9, code[1] = 0x11, code[2] = 0x22, code[3] = 0x33, code[4] = 0x44;
            CHECK(rbk_apply(&s, &rbk_defs[RBK_EPEND], &MEM, 1) == 0 && strstr(s.why, "e9 11 22 33 44") != NULL);
        }
        {   /* memory trouble */
            mem_reset();
            rbk_state s;
            memset(&s, 0, sizeof s);
            fail_read = 1;
            CHECK(rbk_apply(&s, &rbk_defs[RBK_PCS], &MEM, 1) == 0 && s.st == RBK_ERROR && strstr(s.why, "cannot be read"));
            fail_read = 0, fail_alloc = 1;
            CHECK(rbk_apply(&s, &rbk_defs[RBK_PCS], &MEM, 1) == 0 && s.st == RBK_ERROR && strstr(s.why, "executable memory") && !memcmp(loc(0x8916E0u, 6), rbk_defs[RBK_PCS].sig, 6));
            fail_alloc = 0, fail_write = 1;
            CHECK(rbk_apply(&s, &rbk_defs[RBK_PCS], &MEM, 1) == 0 && s.st == RBK_ERROR && strstr(s.why, "write"));
            fail_write = 0;
            CHECK(rbk_apply(&s, &rbk_defs[RBK_PCS], &MEM, 1) == 1 && s.st == RBK_ON);   /* retry works */
            /* an overwritten detour: apply and revert refuse */
            uint8_t *code = loc(0x8916E0u, 6);
            code[0] = 0x90;
            CHECK(rbk_apply(&s, &rbk_defs[RBK_PCS], &MEM, 1) == 0 && strstr(s.why, "overwritten"));
            s.st = RBK_ON;
            CHECK(rbk_revert(&s, &rbk_defs[RBK_PCS], &MEM) == 0 && strstr(s.why, "foreign"));
        }
        {   /* the stub fits its block and the PRE layout puts the early return where the table says */
            uint8_t out[160];
            for (int i = 0; i < RBK_N; i++) {
                int n = rbk_stub_build(out, sizeof out, &rbk_defs[i], 0x70000000u, 0x12345678u, 0x70000068u);
                CHECK(n > 20 && n <= 100);
            }
            CHECK(rbk_stub_build(out, 50, &rbk_defs[0], 0x70000000u, 1, 2) == 0);
            int n = rbk_stub_build(out, sizeof out, &rbk_defs[RBK_EPEND], 0x70000000u, 0x12345678u, 0);
            CHECK(out[n - 5] == 0x31 && out[n - 4] == 0xC0 && out[n - 3] == 0xC2 && out[n - 2] == 0x04 && out[n - 1] == 0x00);
            n = rbk_stub_build(out, sizeof out, &rbk_defs[RBK_RESTART], 0x70000000u, 0x12345678u, 0);
            CHECK(out[n - 3] == 0xC2 && out[n - 2] == 0x0C && out[n - 1] == 0x00);
            n = rbk_stub_build(out, sizeof out, &rbk_defs[RBK_PCS], 0x70000000u, 0x12345678u, 0);
            CHECK(out[n - 5] == 0xE9);   /* observers end with the jump back, no early return */
        }
    }

    /* ============================================ 3. messages and triples */
    {
        uint8_t m[RB_MSG_MAX];
        sim_start();
        CHECK(rb_msg_arrive(m, sizeof m, RB_SECTION, 0xCAFEBABEu, 7) == 9);
        CHECK(rb_on_event(&P[1].b, m, 9) == 1 && P[1].b.arrivals_rx == 1);
        CHECK(rb_on_event(&P[1].b, m, 9) == 1 && P[1].b.arrivals_rx == 2);   /* a duplicate is accepted and stored once */
        int used = 0;
        for (int i = 0; i < 32; i++) used += P[1].b.peer[i].used;
        CHECK(used == 1);
        CHECK(rb_msg_arrive(m, 5, RB_SECTION, 1, 1) == 0 && rb_msg_arrive(m, sizeof m, 9, 1, 1) == 0);
        uint8_t bad[16];
        memset(bad, 0, sizeof bad);
        CHECK(rb_on_event(&P[1].b, bad, 9) == 0);                       /* version 0 */
        rb_msg_arrive(bad, sizeof bad, RB_PCS, 5, 5), bad[2] = 7;
        CHECK(rb_on_event(&P[1].b, bad, 9) == 0);                       /* unknown kind */
        rb_msg_arrive(bad, sizeof bad, RB_PCS, 5, 5);
        CHECK(rb_on_event(&P[1].b, bad, 8) == 0 && rb_on_event(&P[1].b, bad, 10) == 0 && rb_on_event(&P[1].b, bad, 1) == 0 && rb_on_event(&P[1].b, bad, 0) == 0);
        bad[1] = 9;
        CHECK(rb_on_event(&P[1].b, bad, 9) == 0);                       /* unknown sub */
        P[1].b.enabled = 0;
        rb_msg_arrive(bad, sizeof bad, RB_PCS, 5, 5);
        CHECK(rb_on_event(&P[1].b, bad, 9) == 0);                       /* disabled: nothing is taken */
        P[1].b.enabled = 1;

        rb_triple t = {0x91010200u, 1, 2}, bt;
        char why[100];
        CHECK(rb_triple_ok(&t, why, sizeof why));
        t.area = 0x91010100u;
        CHECK(rb_triple_ok(&t, why, sizeof why));
        t.area = 0x91FF0000u; CHECK(!rb_triple_ok(&t, why, sizeof why) && strstr(why, "section"));
        t.area = 0x92010100u; CHECK(!rb_triple_ok(&t, why, sizeof why));
        t.area = 0x91010101u; CHECK(!rb_triple_ok(&t, why, sizeof why));
        t.area = 0x91000100u; CHECK(!rb_triple_ok(&t, why, sizeof why));
        t.area = 0x91010100u, t.a = 0, t.b = 0;  CHECK(!rb_triple_ok(&t, why, sizeof why) && strstr(why, "zero"));
        t.a = -2, t.b = 1; CHECK(!rb_triple_ok(&t, why, sizeof why) && strstr(why, "range"));
        t.a = 1, t.b = 0x10000; CHECK(!rb_triple_ok(&t, why, sizeof why));
        t.a = 0, t.b = 3; CHECK(rb_triple_ok(&t, why, sizeof why));
        t.a = -1, t.b = 2; CHECK(rb_triple_ok(&t, NULL, 0));

        CHECK(rb_msg_restart(m, sizeof m, 3, 0x91010200u, 1, 2) == 16 && rb_msg_restart(m, 15, 3, 1, 1, 2) == 0);
        bt = (rb_triple){0, 0, 0};
        CHECK(rb_on_event(&P[0].b, m, 16) == 0);                        /* the host ignores a RESTART */
        CHECK(!P[0].b.rs_have);
        CHECK(rb_on_event(&P[1].b, m, 16) == 1 && P[1].b.rs_have && P[1].b.rs_area == 0x91010200u && P[1].b.rs_a == 1 && P[1].b.rs_b == 2 && P[1].b.rs_seq == 3);
        P[1].b.rs_have = 0;
        rb_msg_restart(m, sizeof m, 4, 0x91FF0000u, 1, 2);
        CHECK(rb_on_event(&P[1].b, m, 16) == 0 && !P[1].b.rs_have && P[1].b.rs_rejected == 1);
        rb_msg_restart(m, sizeof m, 4, 0x91010200u, 0, 0);
        CHECK(rb_on_event(&P[1].b, m, 16) == 0 && P[1].b.rs_rejected == 2);
        CHECK(rb_hash("") == 0 && rb_hash(NULL) == 0 && rb_hash("11_028_0") != 0 && rb_hash("11_028_0") != rb_hash("11_100_0") && rb_hash("a") == rb_hash("a"));
        (void)bt;
    }

    /* ============================================ 4. the rendezvous */
    {   /* A arrives first and is held until B arrives (and the message gets there); B is released at once */
        sim_start();
        S.phase = 3;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD);
        for (int i = 0; i < 100; i++) { tick(1); CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD); }
        CHECK(P[0].b.arrivals_tx == 1 && P[0].b.holds >= 100);
        tick(500);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD);       /* the other PC has not arrived: still held (frames come each ms, the latch never lapses) */
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);    /* B: A's arrival is already there */
        CHECK(P[1].b.released_peer == 1);
        tick(1);
        int rel = 0;
        for (int i = 0; i < 40 && !rel; i++) { tick(1); rel = rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED; }
        CHECK(rel && P[0].b.released_peer == 1);
        for (int i = 0; i < 20; i++) { tick(5); CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED && rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED); }   /* the state stays released while the engine keeps asking */
        CHECK(P[0].b.arrivals_tx == 1 && P[1].b.arrivals_tx == 1);   /* one arrival message each, however often the gate is asked */
        /* the second section end later is a new meeting (n = 2) */
        tick(5000);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD && P[0].b.pt[RB_SECTION].n == 2);
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_HOLD);       /* B arrives; A's message is still on the line (20 ms): both hold... */
        tick(25);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED && rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);   /* ... and meet */
    }
    {   /* both arrive in the same millisecond with a slow, duplicating, reordering line: nobody is released before the other one is there, nobody waits for ever */
        for (int variant = 0; variant < 3; variant++) {
            sim_start();
            S.phase = 3;
            line_dup = variant >= 1, line_reorder = variant >= 2, line_delay = variant == 0 ? 20 : 80;
            int r0 = 0, r1 = 0;
            uint32_t t_free0 = 0, t_free1 = 0;
            for (int i = 0; i < 400; i++) {
                tick(1);
                if (!r0 && rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED) r0 = 1, t_free0 = T;
                if (!r1 && rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED) r1 = 1, t_free1 = T;
            }
            CHECK(r0 && r1);
            CHECK(t_free0 >= 100001u + line_delay - 1 && t_free1 >= 100001u + line_delay - 1);   /* never before the message could have come */
            CHECK(P[0].b.released_timeout == 0 && P[1].b.released_timeout == 0);
        }
    }
    {   /* the other PC never comes: the timeout releases, once, with the reason in the log */
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        uint32_t t0 = T;
        int r = 0;
        while (!r && T - t0 < 200000) { tick(10); r = rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED; }
        CHECK(r && T - t0 >= 180000 && T - t0 < 180100 && P[0].b.released_timeout == 1);
        CHECK(strstr(logbuf, "TIMEOUT") != NULL);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED);
        /* a cutscene end has the shorter limit */
        t0 = T;
        P[0].b.pt[RB_PCS].st = 0;
        r = 0;
        rb_pcs_begin(&P[0].b, rb_hash("11_028_0"), 1);
        while (!r && T - t0 < 100000) { tick(10); r = rb_pcs_end(&P[0].b, 1) == 1; }
        CHECK(r && T - t0 >= 60000 && T - t0 < 60100 && P[0].b.released_timeout == 2);
    }
    {   /* no session / disabled / cannot send: always PROCEED, never a hold */
        sim_start();
        S.started = 0;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED && strstr(P[0].b.why, "no network session"));
        S.started = 1;
        P[0].b.enabled = 0;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED);
        CHECK(rb_pcs_end(&P[0].b, 1) == 1 && rb_pcs_end(&P[0].b, 0) == 0);
        P[0].b.enabled = 1;
        P[0].sends_ok = 0;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED && strstr(P[0].b.why, "could not be sent"));
        P[0].sends_ok = 1;
        S.started = 0;                                           /* the session ends while held */
        sim_start();
        S.phase = 3;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD);
        S.started = 0;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED);
        rb_reset(&P[0].b);
        CHECK(P[0].b.pt[RB_SECTION].st == 0 && P[0].b.pcs_pending == 0);
    }
    {   /* cutscenes: the script sees "ended" only when both ended; not-ended and no-begin cases pass through */
        sim_start();
        S.phase = 3;
        uint32_t k = rb_hash("11_100_0");
        CHECK(rb_pcs_end(&P[0].b, 0) == 0 && rb_pcs_end(&P[0].b, 1) == 1);   /* no begin seen: untouched */
        rb_pcs_begin(&P[0].b, k, 1);
        rb_pcs_begin(&P[1].b, k, 1);
        CHECK(P[0].b.pcs_begins == 1);
        CHECK(rb_pcs_end(&P[0].b, 0) == 0);                                  /* still playing: the game's own answer */
        CHECK(P[0].b.arrivals_tx == 0);                                      /* nothing sent while it plays */
        CHECK(rb_pcs_end(&P[0].b, 1) == 0);                                  /* ended here, held */
        tick(100);
        CHECK(rb_pcs_end(&P[0].b, 1) == 0);
        CHECK(rb_pcs_end(&P[1].b, 0) == 0);                                  /* B still plays */
        CHECK(rb_pcs_end(&P[1].b, 1) == 1);                                  /* B ended later: A's arrival is there */
        tick(40);
        CHECK(rb_pcs_end(&P[0].b, 1) == 1);                                  /* A released */
        CHECK(P[0].b.pcs_pending == 0 && rb_pcs_end(&P[0].b, 1) == 1 && rb_pcs_end(&P[0].b, 0) == 0);   /* afterwards: pass through */
        /* the same name again is the second meeting; a different name is its own */
        tick(4000);
        rb_pcs_begin(&P[0].b, k, 1);
        rb_pcs_begin(&P[1].b, k, 1);
        CHECK(rb_pcs_end(&P[0].b, 1) == 0 && P[0].b.pt[RB_PCS].n == 2);
        rb_pcs_begin(&P[1].b, rb_hash("11_050_0"), 1);
        CHECK(rb_pcs_end(&P[1].b, 1) == 0 && P[1].b.pt[RB_PCS].n == 1);
        tick(60);
        CHECK(rb_pcs_end(&P[0].b, 1) == 0);                                  /* B arrived for ANOTHER cutscene: A is not released by it */
        /* kinds do not mix: a section arrival does not release a cutscene */
        sim_start();
        S.phase = 3;
        rb_pcs_begin(&P[0].b, 0, 1);
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_HOLD);
        tick(30);
        CHECK(rb_pcs_end(&P[0].b, 1) == 0);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED);
        rb_movie_begin(&P[0].b, 1);
        rb_section_end_seen(&P[0].b);
        CHECK(P[0].b.movie_begins == 1 && P[0].b.section_ends == 1);
    }
    /* ep1-probe e1p1 (2026-11-08 09:15-09:20 live): the host's 11_10b_0 ended at once, the client's copy 33 s later (its own prompt press); the host's mPcsIsEnd
     * said "ended" only every 8.3 s (the wave loop's cadence), so every read came after the 3 s latch: occurrence #1..#31, the client's arrival #1 matched none,
     * the 60 s timeout restarted with each and the host's FSM never saw the end (no movie 11_088_0, no Ooze on the host). With the fix a pending cutscene keeps its
     * occurrence: the late arrival releases it; without the partner the timeout does. The model before the fix (pcs_relatch 1) fails both. */
    for (int model = 0; model < 2; model++) {
        int bad = 0;
        uint32_t k = rb_hash("demo\\d0\\11_10b_0\\11_10b_0_main");
        for (int part = 0; part < 2; part++) {   /* part 0: the partner ends 33 s late; part 1: the partner never ends */
            sim_start();
            S.phase = 3;
            P[0].b.cfg.pcs_relatch = P[1].b.cfg.pcs_relatch = model;
            rb_pcs_begin(&P[0].b, k, 1);
            rb_pcs_begin(&P[1].b, k, 1);
            uint32_t t0 = T;
            int released = 0, b_ended = 0;
            while (!released && T - t0 < 150000) {
                int read = ((T - t0) % 8300u) < 10u;   /* the host's getter answers "ended" only in this window of each 8.3 s */
                if (read && rb_pcs_end(&P[0].b, 1) == 1) released = 1;
                if (part == 0 && !b_ended && T - t0 >= 33000) b_ended = rb_pcs_end(&P[1].b, 1) == 1 || 1;
                tick(10);
            }
            uint32_t took = T - t0;
            int ok = part == 0 ? released && took < 42000 && P[0].b.k_peer[RB_PCS] == 1 && P[0].b.pt[RB_PCS].n == 1   /* the next read after B's arrival */
                               : released && took >= 60000 && took < 70000 && P[0].b.released_timeout == 1;
            if (model == 0) CHECK(ok);
            bad += !ok;
        }
        if (model == 1) {
            CHECK(bad == 2);   /* the model before the fix must fail both */
            printf("  cutscene end read every 8.3 s: the model before the fix (re-latch per read) fails %d of 2 checks (never released)\n", bad);
        }
    }

    /* ============================================ 4b. behaviour-neutral while nothing meets (live 2026-11-06: two instances, no control in the opening) */
    {
        /* only demo\ resources are cutscenes; the scene scripts of 1-1 seen live (the wave loop restarts every 8.3 s) are not */
        CHECK(rb_pcs_is_cutscene("demo\\d0\\11_103_0\\11_103_0_main") && rb_pcs_is_cutscene("DEMO/d0/x") && rb_pcs_is_cutscene("Demo\\x"));
        CHECK(!rb_pcs_is_cutscene("game\\scene\\11\\11_00\\11_00_s100_wavemain") && !rb_pcs_is_cutscene("game\\scene\\11\\11_00\\11_00_s100_rainmain_Parker"));
        CHECK(!rb_pcs_is_cutscene("") && !rb_pcs_is_cutscene(NULL) && !rb_pcs_is_cutscene("demox\\a") && !rb_pcs_is_cutscene("dem"));
        sim_start();
        S.phase = 3;
        /* the opening of 1-1 as logged: four scene scripts, then the wave loop again and again: nothing is armed, nothing held, the getter's answers pass bit for bit */
        static const char *const opening[] = {"game\\scene\\11\\11_00\\11_00_s100_drawoffmain", "game\\scene\\11\\11_00\\11_00_s100_wavemain", "game\\scene\\11\\11_00\\11_00_s100_rainmain",
                                              "game\\scene\\11\\11_00\\11_00_s100_rainmain_Parker"};
        for (int i = 0; i < 4; i++) rb_pcs_begin(&P[0].b, rb_hash(opening[i]), rb_pcs_is_cutscene(opening[i]));
        static const uint32_t answers[] = {0, 1, 0xFFFFFF01u, 0x00000100u, 0x00000201u, 0x12345600u, 0xFFFFFFFFu};
        for (int round = 0; round < 50; round++) {
            rb_pcs_begin(&P[0].b, rb_hash(opening[1]), 0);
            for (int a = 0; a < 7; a++) CHECK(rb_pcs_filter(&P[0].b, answers[a]) == answers[a]);
            tick(300);
        }
        CHECK(P[0].b.pcs_pending == 0 && P[0].b.pcs_ambient == 54 && P[0].b.pcs_begins == 54);
        CHECK(P[0].b.arrivals_tx == 0 && P[0].b.holds == 0 && P[0].b.pcs_changed == 0 && P[0].b.k_episodes[RB_PCS] == 0);
        /* the getter's eax is handed on unchanged after a pass too (pending cleared by a release / a pass without session) */
        S.started = 0;
        rb_pcs_begin(&P[0].b, rb_hash("demo\\d0\\11_100_0\\11_100_0_main"), 1);
        CHECK(P[0].b.pcs_pending == 1);
        logbuf[0] = 0;
        CHECK(rb_pcs_filter(&P[0].b, 0xFFFFFF01u) == 0xFFFFFF01u && P[0].b.pcs_pending == 0 && P[0].b.k_nosession[RB_PCS] == 1);
        CHECK(strstr(logbuf, "with no network session") != NULL);   /* the silent pass is in the log now */
        size_t ll = strlen(logbuf);
        for (int i = 0; i < 10; i++) rb_gate(&P[0].b, RB_PCS, 1);
        CHECK(strlen(logbuf) == ll && P[0].b.k_nosession[RB_PCS] == 11);   /* ... once, not every frame */
        S.started = 1;
        /* a real hold replaces only the low byte, and is logged at its start and at its end */
        rb_pcs_begin(&P[0].b, rb_hash("demo\\d0\\11_100_0\\11_100_0_main"), 1);
        rb_pcs_begin(&P[1].b, rb_hash("demo\\d0\\11_100_0\\11_100_0_main"), 1);
        logbuf[0] = 0;
        CHECK(rb_pcs_filter(&P[0].b, 0xFFFFFF00u) == 0xFFFFFF00u);           /* not ended: untouched, nothing sent */
        CHECK(P[0].b.arrivals_tx == 0);
        CHECK(rb_pcs_filter(&P[0].b, 0xFFFFFF01u) == 0xFFFFFF00u && P[0].b.pcs_changed == 1);   /* ended here, held: "not ended" with the same upper bits */
        CHECK(strstr(logbuf, "HOLD begins") != NULL && P[0].b.k_episodes[RB_PCS] == 1);
        for (int i = 0; i < 5; i++) { tick(16); CHECK(rb_pcs_filter(&P[0].b, 1) == 0); }
        CHECK(P[0].b.k_episodes[RB_PCS] == 1 && P[0].b.k_holds[RB_PCS] == 6 && P[0].b.pcs_changed == 6);
        CHECK(rb_pcs_filter(&P[1].b, 1) == 1 && strstr(logbuf, "passed at once") != NULL);   /* the other PC: A's arrival was there, no hold */
        CHECK(P[1].b.k_episodes[RB_PCS] == 0 && P[1].b.pcs_changed == 0);
        tick(40);
        CHECK(rb_pcs_filter(&P[0].b, 0x00000201u) == 0x00000201u && strstr(logbuf, "RELEASED by the other PC") != NULL && P[0].b.k_peer[RB_PCS] == 1);
        /* a section end without the other PC: logged hold start, then TIMEOUT with the frame count */
        logbuf[0] = 0;
        P[0].b.cfg.section_ms = 1000;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD && strstr(logbuf, "section end #1 HOLD begins"));
        while (rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD) tick(16);
        CHECK(strstr(logbuf, "RELEASED by the TIMEOUT") != NULL && P[0].b.k_timeout[RB_SECTION] == 1 && P[0].b.k_episodes[RB_SECTION] == 1 && P[0].b.k_holds[RB_SECTION] >= 60);
        char st[2400];
        rb_status(&P[0].b, st, sizeof st);
        CHECK(strstr(st, "getter answers replaced 6") != NULL && strstr(st, "scene scripts, never held: 54") != NULL);
    }

    /* ============================================ 4b. the section end gate (ef4 live, 2026-11-08) */
    {   /* cEpisodeEndState_vf7 runs ONCE per section end (`barrier status`: epend calls 1). The old glue called rb_gate(RB_SECTION): the host arrived first,
         * the early return held vf7, nothing ever called the gate again, and neither the client's arrival nor the timeout could release it: the host stayed in
         * 1-2 while the client went on into 1-3 (then the host's restart crashed the client). Model of the one call per PC: */
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        int one = rb_gate(&P[0].b, RB_SECTION, 0);    /* the old path, one call: HOLD = the section change is lost (no second call comes) */
        CHECK(one == RB_HOLD);
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        CHECK(rb_section_gate(&P[0].b) == RB_PROCEED);   /* the host first, ONE call: no hold */
        CHECK(P[0].b.sec_nohold == 1 && P[0].b.arrivals_tx == 1 && P[0].b.k_holds[RB_SECTION] == 0 && P[0].b.holds == 0);
        CHECK(strstr(logbuf, "section end #1 reached, arrival sent") != NULL && strstr(logbuf, "WITHOUT a hold") != NULL && strstr(logbuf, "HOLD begins") == NULL);
        tick(40);                                         /* the host's arrival reaches the client */
        CHECK(rb_section_gate(&P[1].b) == RB_PROCEED);    /* the client later, ONE call: the host was there first */
        CHECK(P[1].b.k_peer[RB_SECTION] == 1 && P[1].b.sec_nohold == 0 && strstr(logbuf, "the other PC was there first") != NULL);
        /* the host's section change right after its one call: SECTION goes out "without a rendezvous" (how ALONE); the client's own change consumes it */
        CHECK(P[0].b.pt[RB_SECTION].st == 1 && P[1].b.pt[RB_SECTION].st == 2);
        /* a second visit later (the next section end) is a new occurrence */
        tick(5000);
        CHECK(rb_section_gate(&P[0].b) == RB_PROCEED && P[0].b.pt[RB_SECTION].n == 2 && P[0].b.sec_nohold == 2);
        /* no session: goes on as before (rb_gate's no-session path) */
        S.started = 0;
        CHECK(rb_section_gate(&P[1].b) == RB_PROCEED && strstr(P[1].b.why, "no network session"));
        S.started = 1;
        /* the glue calls the one-shot gate, never rb_gate(RB_SECTION) */
        {
            static char src[1 << 20];
            FILE *f = fopen("../native/rolebarrier1_game.c", "rb");
            size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
            if (f) fclose(f);
            src[n] = 0;
            CHECK(n > 10000 && strstr(src, "rb_section_gate(&B)") != NULL && strstr(src, "rb_gate(&B, RB_SECTION") == NULL);
        }
    }

    /* ============================================ 5. the level load barrier */
    {   /* a start with both levels already up: both report loaded, GO */
        sim_start();
        for (int i = 0; i < 1500; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        CHECK(S.phase == 3 && S.gos[0] == 1 && P[0].loads_done_calls == 1 && P[1].loads_done_calls == 1 && P[0].loads_begin_calls == 0);
    }
    {   /* one PC loads much slower: the barrier waits for it and opens when both are playable */
        sim_start();
        int p1_ready_at = 6000;
        for (int i = 0; i < 12000; i++) {
            tick(1);
            rb_load_step(&P[0].b, 1);
            rb_load_step(&P[1].b, i >= p1_ready_at);
            if (i < p1_ready_at - 100) CHECK(S.phase == 2);
        }
        CHECK(S.phase == 3 && P[1].loads_done_calls == 1);
        CHECK(T >= 100000u + 6000u + 500u - 5);                    /* not before the slow PC settled */
    }
    {   /* a section change: both lose the level, begin, come back; the epoch moves once and both are in it */
        sim_start();
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        CHECK(S.phase == 3 && S.epoch == 1);
        for (int i = 0; i < 650; i++) { tick(1); rb_load_step(&P[0].b, 0); rb_load_step(&P[1].b, 0); }   /* both levels gone, not yet long enough */
        CHECK(S.phase == 3 && S.epoch == 1);
        for (int i = 0; i < 100; i++) { tick(1); rb_load_step(&P[0].b, 0); rb_load_step(&P[1].b, 0); }   /* ... now */
        CHECK(S.phase == 2 && S.epoch == 2 && P[0].loads_begin_calls + P[1].loads_begin_calls >= 1);   /* the second PC finds the epoch already moved and joins it (no second advance) */
        for (int i = 0; i < 3000; i++) { tick(1); rb_load_step(&P[0].b, i > 1500); rb_load_step(&P[1].b, i > 1000); }
        CHECK(S.phase == 3 && S.epoch == 2 && P[0].loads_done_calls == 2 && P[1].loads_done_calls == 2);
        /* a flicker of the camera (shorter than the debounce) is no load */
        unsigned e = S.epoch;
        for (int i = 0; i < 1500; i++) { tick(1); rb_load_step(&P[0].b, (i >= 200 && i < 600) ? 0 : 1); rb_load_step(&P[1].b, 1); }   /* gone for 400 ms: no load */
        CHECK(S.epoch == e && S.phase == 3 && P[0].loads_begin_calls + P[1].loads_begin_calls <= 1);
    }
    {   /* only one PC loads (a checkpoint reload on the host): the client sees the new epoch, keeps its level, and is not counted as loaded before its own load or the stale limit */
        sim_start();
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        CHECK(S.phase == 3);
        int open_at = -1;
        for (int i = 0; i < 20000 && open_at < 0; i++) {
            tick(1);
            rb_load_step(&P[0].b, i < 800 ? 0 : (i < 3000 ? 0 : 1));   /* the host's level is away for 3 s */
            rb_load_step(&P[1].b, 1);                                  /* the client's level never goes away */
            if (S.phase == 3 && S.epoch == 2) open_at = i;
        }
        CHECK(open_at >= 8000 - 20 && P[1].b.loads_stale == 1);        /* the client waited for its stale limit (8 s), the host for the client */
        CHECK(open_at < 9500);
        CHECK(strstr(logbuf, "reporting loaded anyway") != NULL);
    }
    {   /* ef4 (live 2026-11-08): the stale fallback while the OTHER PC still loads for minutes: the line came on every frame (host log 17k lines in 3 min) and
         * loads_stale counted frames; once per epoch now */
        sim_start();
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        CHECK(S.phase == 3);
        logbuf[0] = 0;
        for (int i = 0; i < 20000; i++) {
            tick(1);
            rb_load_step(&P[0].b, i < 800 ? 1 : 0);   /* the host's level goes away at 0.8 s and stays away (a long load) */
            rb_load_step(&P[1].b, 1);                  /* the client's level stays */
        }
        int nstale = 0;
        for (const char *q = logbuf; (q = strstr(q, "reporting loaded anyway")) != NULL; q++) nstale++;
        CHECK(S.phase == 2 && P[1].b.loads_stale == 1 && nstale == 1 && P[1].loads_done_calls == 2);
        if (nstale != 1 || P[1].b.loads_stale != 1) printf("  (stale lines %d, loads_stale %u)\n", nstale, P[1].b.loads_stale);
    }
    {   /* a client whose level does go away: reports at its own pace, no stale fallback */
        sim_start();
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        for (int i = 0; i < 6000; i++) {
            tick(1);
            rb_load_step(&P[0].b, i > 2500 ? 1 : 0);                   /* host: away 0..2500 */
            rb_load_step(&P[1].b, (i > 1000 && i < 1900) ? 0 : 1);     /* client: away 1000..1900, after the host's epoch started */
        }
        CHECK(S.phase == 3 && S.epoch == 2 && P[1].b.loads_stale == 0 && P[1].loads_done_calls == 2);
    }
    {   /* session end resets the load logic */
        sim_start();
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        S.started = 0;
        rb_load_step(&P[0].b, 1);
        CHECK(P[0].b.first_load == 1 && !P[0].b.epoch_valid);
        P[0].b.enabled = 0;
        S.started = 1;
        unsigned d0 = P[0].loads_done_calls, b0 = P[0].loads_begin_calls;
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, i % 1500 < 700); }
        CHECK(P[0].loads_done_calls == d0 && P[0].loads_begin_calls == b0);   /* disabled: never touches the session */
    }

    /* ============================================ 5b. e1w2 live: the units wait at 0,0,0 through the load / the start movie */
    CHECK(!rb_placed(0, 0, 0) && !rb_placed(0.3f, 0.3f, 0.3f) && rb_placed(-300.0f, -200.0f, 18100.0f) && rb_placed(0, 0, 2.0f));
    {
        float zero = 0.0f, nan = zero / zero, inf = 1.0f / zero;
        CHECK(!rb_placed(nan, 1000, 1000) && !rb_placed(inf, 0, 0) && !rb_placed(5e7f, 0, 0));
    }
    {   /* the first load: the barrier opens only when both humans are placed (the glue before the fix opened it 0.9 s after the restart call) */
        int o_new = e1w2_first_load(pred_new, 110000), o_old = e1w2_first_load(pred_old, 110000);
        CHECK(o_new >= 110300 + 500 - 5 && o_new < 110300 + 1500);
        CHECK(o_old >= 0 && o_old < 2000);   /* the model before the fix fails the check above (e1w2: LOAD_DONE 1.2 s after the restart, the level 110 s later) */
        printf("  5b first load: barrier open %d ms after the restart call (before the fix: %d ms)\n", o_new, o_old);
        int o_quick = e1w2_first_load(pred_new, 900);   /* no movie: as before */
        CHECK(o_quick >= 900 + 300 + 500 - 5 && o_quick < 3000);
    }
    {   /* an in-session restart: a new epoch at once, the barrier reopens when both are placed (before the fix: one epoch for both levels) */
        int ea, oa, early;
        e1w2_restart(pred_new, &ea, &oa, &early);
        CHECK(ea >= 370 && ea < 370 + 700 + 200 && !early && oa >= 200000 + 500 - 5 && oa < 200000 + 1500);
        printf("  5b restart: new epoch %d ms after the restart, barrier open again at %d ms\n", ea, oa);
        e1w2_restart(pred_old, &ea, &oa, &early);
        CHECK(ea < 0);   /* the model before the fix never saw the level go away */
    }

    /* ============================================ 6. Retry / Continue */
    {
        sim_start();
        S.phase = 3;
        rb_triple host_t = {0x91010200u, 2, 1}, own = {0x91010100u, 1, 2};
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1 && P[0].b.rs_sent == 1);
        CHECK(P[1].b.rs_have == 0);
        tick(30);
        CHECK(P[1].b.rs_have == 1 && P[1].b.rs_received == 1);
        rb_triple t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == 1 && t.area == 0x91010200u && t.a == 2 && t.b == 1 && !P[1].b.rs_have && P[1].b.rs_applied == 1);
        t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == 0 && t.area == own.area && t.a == own.a && P[1].b.rs_unmatched == 1);   /* one triple, one restart */
        /* a second host restart replaces an unused one; seq grows */
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1);
        host_t.a = 3;
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1 && P[0].b.rs_tx_seq == 3);
        tick(30);
        t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == 1 && t.a == 3);
        /* an old triple is dropped */
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1);
        tick(30 + 31000);
        t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == 0 && strstr(logbuf, "dropped") != NULL);
        /* an invalid triple of the host is not sent */
        rb_triple bad = {0x91FF0000u, 1, 1};
        unsigned sent = P[0].b.rs_sent;
        CHECK(rb_restart_host(&P[0].b, &bad) == 0 && P[0].b.rs_sent == sent);
        /* no session: nothing is sent, the client keeps its own */
        S.started = 0;
        CHECK(rb_restart_host(&P[0].b, &host_t) == 0);
        t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == 0 && strstr(P[1].b.why, "no network session"));
        S.started = 1;
        /* the hold policy: "not now" for the limit, then the own checkpoint */
        P[1].b.cfg.no_triple_hold_ms = 1000;
        P[1].b.rs_wait_t0 = 0;
        t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == -1 && t.area == own.area);
        tick(500);
        CHECK(rb_restart_client(&P[1].b, &t) == -1);
        tick(600);
        CHECK(rb_restart_client(&P[1].b, &t) == 0 && t.area == own.area);
        /* the triple arrives while it is holding: used at the next call */
        P[1].b.rs_wait_t0 = 0;
        CHECK(rb_restart_client(&P[1].b, &t) == -1);
        host_t.a = 2;
        rb_restart_host(&P[0].b, &host_t);
        tick(30);
        CHECK(rb_restart_client(&P[1].b, &t) == 1 && t.a == 2 && P[1].b.rs_wait_t0 == 0);
        /* console injection */
        rb_triple inj = {0x91010300u, 5, 6};
        rb_restart_inject(&P[1].b, &inj);
        t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == 1 && t.area == 0x91010300u && t.a == 5 && t.b == 6);
        char st[2000];
        rb_status(&P[1].b, st, sizeof st);
        CHECK(strstr(st, "restart:") != NULL && strstr(st, "rendezvous ON") != NULL);
    }

    /* ============================================ 6b. the client follows the host's restart (world-downed; wp1 live: only the host reloaded) */
    {
        sim_start();
        S.phase = 3;
        rb_triple host_t = {0x91090200u, 273, 13}, out = {0, 0, 0}, own = {0x91090200u, 1, 2};
        CHECK(P[1].b.cfg.follow_ms == 1500);   /* default */
        CHECK(rb_restart_follow(&P[1].b, &out) == 0);   /* nothing waiting */
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1);
        tick(30);
        CHECK(P[1].b.rs_have == 1 && rb_restart_follow(&P[1].b, &out) == 0);   /* too young: an own Continue may still come */
        CHECK(rb_restart_follow(&P[0].b, &out) == 0);                             /* never on the host */
        tick(1500);
        CHECK(rb_restart_follow(&P[1].b, &out) == 1 && out.area == 0x91090200u && out.a == 273 && out.b == 13 && P[1].b.rs_followed == 1);
        CHECK(rb_restart_follow(&P[1].b, &out) == 0 && P[1].b.rs_followed == 1);   /* once per triple */
        rb_triple t = own;   /* the engine call that follows goes through the restart hook: it takes the same triple */
        CHECK(rb_restart_client(&P[1].b, &t) == 1 && t.a == 273 && t.b == 13 && !P[1].b.rs_have);
        CHECK(rb_restart_follow(&P[1].b, &out) == 0);
        /* the client's own Continue uses the triple first: no follow afterwards */
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1);
        tick(30);
        t = own;
        CHECK(rb_restart_client(&P[1].b, &t) == 1);
        tick(2000);
        CHECK(rb_restart_follow(&P[1].b, &out) == 0 && P[1].b.rs_followed == 1);
        /* a new triple (next seq) may be followed again */
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1);
        tick(1600);
        CHECK(rb_restart_follow(&P[1].b, &out) == 1 && P[1].b.rs_followed == 2);
        t = own;
        rb_restart_client(&P[1].b, &t);
        /* switched off, too old, no session, disabled */
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1);
        tick(30);
        P[1].b.cfg.follow_ms = 0;
        tick(2000);
        CHECK(rb_restart_follow(&P[1].b, &out) == 0);
        P[1].b.cfg.follow_ms = 1500;
        tick(31000);
        CHECK(rb_restart_follow(&P[1].b, &out) == 0);   /* older than the freshness limit */
        CHECK(rb_restart_host(&P[0].b, &host_t) == 1);
        tick(1600);
        S.started = 0;
        CHECK(rb_restart_follow(&P[1].b, &out) == 0);
        S.started = 1;
        P[1].b.enabled = 0;
        CHECK(rb_restart_follow(&P[1].b, &out) == 0);
        P[1].b.enabled = 1;
        CHECK(rb_restart_follow(&P[1].b, &out) == 1);
        rb_reset(&P[1].b);
        CHECK(P[1].b.rs_follow_seq == 0 && P[1].b.rs_have == 0);
        char st[2000];
        rb_status(&P[1].b, st, sizeof st);
        CHECK(strstr(st, "followed") != NULL);
    }

    /* ============================================ 6c. lobby-must: the host's story start (FUN_00720540 observer), the Episode 1 gate, the follow at the title */
    {
        /* the definition and the bytes of the build (from the image when it is at hand) */
        static const uint8_t sg[6] = {0x53, 0x8B, 0xD9, 0x56, 0x85, 0xDB};
        CHECK(rbk_defs[RBK_START].va == 0x720540u && !memcmp(rbk_defs[RBK_START].sig, sg, 6) && rbk_defs[RBK_START].steal == 6 && rbk_defs[RBK_START].hold_ret == -1 &&
              rbk_defs[RBK_START].kind == RBK_PRE && !strcmp(rbk_defs[RBK_START].name, "start"));
        {
            const char *path = getenv("REV1_IMAGE");
            FILE *f = fopen(path && *path ? path : "../../image/rerev-image.bin", "rb");
            if (f) {
                for (int i = 0; i < RBK_N; i++) {   /* every hook's bytes are the build's (image base 0x400000) */
                    uint8_t cur[RBK_SIG_MAX];
                    CHECK(fseek(f, (long)(rbk_defs[i].va - 0x400000u), SEEK_SET) == 0 && fread(cur, 1, (size_t)rbk_defs[i].steal, f) == (size_t)rbk_defs[i].steal);
                    CHECK(!memcmp(cur, rbk_defs[i].sig, (size_t)rbk_defs[i].steal));
                }
                /* the callers of FUN_00720540 inside cEpisodeEndState are in the section-change range, the title's are not */
                static const uint32_t sec[] = {0x42D477u, 0x42D4ADu, 0x42D4E8u, 0x42D523u, 0x42D5A9u, 0x42D5EAu}, title[] = {0x6D0DE9u, 0x6D1183u, 0x6D11D0u, 0x83BEF9u, 0x83C0CCu, 0x83E896u};
                for (unsigned i = 0; i < 6; i++) {
                    uint8_t c[5];
                    CHECK(fseek(f, (long)(sec[i] - 0x400000u), SEEK_SET) == 0 && fread(c, 1, 5, f) == 5 && c[0] == 0xE8);
                    int32_t rel;
                    memcpy(&rel, c + 1, 4);
                    CHECK(sec[i] + 5u + (uint32_t)rel == 0x720540u && sec[i] + 5u >= RB_SECTION_FN_LO && sec[i] + 5u < RB_SECTION_FN_HI);
                    CHECK(fseek(f, (long)(title[i] - 0x400000u), SEEK_SET) == 0 && fread(c, 1, 5, f) == 5 && c[0] == 0xE8);
                    memcpy(&rel, c + 1, 4);
                    CHECK(title[i] + 5u + (uint32_t)rel == 0x720540u && !(title[i] + 5u >= RB_SECTION_FN_LO && title[i] + 5u < RB_SECTION_FN_HI));
                }
                fclose(f);
                printf("ok   image: the bytes of all %d hooks, the 12 callers of FUN_00720540 (6 section change, 6 starts)\n", RBK_N);
            } else
                printf("(no executable image: the hook bytes are not compared with the build)\n");
        }
        CHECK(rb_area_ok(0x91010100u) && rb_area_ok(0x91010300u) && rb_area_ok(0x91020100u) && rb_area_ok(0x91090200u) && !rb_area_ok(0x91010000u) && !rb_area_ok(0x9101FF00u));   /* (allep1: every paired section, any episode) */
        /* lm2 live: the start triple from the start table (the record held no start a frame after the title's New Game) */
        {
            tm_n = 0;
            tm_put(0x1169108u, 0x5000u);                  /* gamesys */
            tm_put(0x5024u, 0x6000u);                     /* o */
            tm_put(0x6020u, 0u);                          /* [o+0x20] == 0: the table at [o+0xC] */
            tm_put(0x600Cu, 0x7000u), tm_put(0x6010u, 0x7800u);
            tm_put(0x706Cu, 3u), tm_put(0x7078u, 0x8000u);   /* 3 entries, the second empty */
            tm_put(0x8000u, 0x9000u), tm_put(0x8004u, 0u), tm_put(0x8008u, 0x9100u);
            tm_put(0x9004u, 0x91020100u), tm_put(0x9008u, 0x92020100u), tm_put(0x900Cu, 3u), tm_put(0x9010u, 4u);
            tm_put(0x9104u, 0x91010100u), tm_put(0x9108u, 0x92010100u), tm_put(0x910Cu, 256u), tm_put(0x9110u, 1u);   /* lm2b live: 0x92010100 256 / 1 */
            tm_put(0x786Cu, 1u), tm_put(0x7878u, 0x8800u), tm_put(0x8800u, 0x9200u);   /* the other table ([o+0x20] != 0) */
            tm_put(0x9204u, 0x91010100u), tm_put(0x9208u, 0x92010100u), tm_put(0x920Cu, 0x21u), tm_put(0x9210u, 0x22u);
            rb_triple tt = {0, 0, 0};
            /* the start id 0x92EESS00 of the record is not a restart area: the triple is the 0x91 key with the entry's checkpoints (as the title Continue calls 720870) */
            rb_triple rec92 = {0x92010100u, 256, 1};
            CHECK(!rb_triple_ok(&rec92, NULL, 0));
            CHECK(rb_table_triple(tm_rd, NULL, 0x1169108u, 0x91010100u, &tt) == 1 && tt.area == 0x91010100u && tt.a == 256 && tt.b == 1 && rb_triple_ok(&tt, NULL, 0));
            CHECK(rb_table_triple(tm_rd, NULL, 0x1169108u, 0x91020100u, &tt) == 1 && tt.area == 0x91020100u && tt.a == 3 && tt.b == 4);
            CHECK(rb_table_triple(tm_rd, NULL, 0x1169108u, 0x91050100u, &tt) == 0);   /* not in the table */
            tm_put(0x6020u, 1u);
            CHECK(rb_table_triple(tm_rd, NULL, 0x1169108u, 0x91010100u, &tt) == 1 && tt.a == 0x21 && tt.b == 0x22);
            tm_put(0x786Cu, 100000u);
            CHECK(rb_table_triple(tm_rd, NULL, 0x1169108u, 0x91010100u, &tt) == 0);   /* an absurd count is refused */
            tm_put(0x5024u, 0u);
            CHECK(rb_table_triple(tm_rd, NULL, 0x1169108u, 0x91010100u, &tt) == 0 && rb_table_triple(NULL, NULL, 0x1169108u, 0x91010100u, &tt) == 0);
            /* the build's code that the reader mirrors (image base 0x400000) */
            const char *path = getenv("REV1_IMAGE");
            FILE *f = fopen(path && *path ? path : "../../image/rerev-image.bin", "rb");
            if (f) {
                static const struct { uint32_t va; uint8_t n; uint8_t b[27]; } code[] = {
                    {0x720570u, 9, {0x8B, 0x51, 0x24, 0x83, 0x7A, 0x20, 0x00, 0x75, 0x30}},                         /* o = [g+0x24]; cmp [o+0x20], 0 */
                    {0x7205A3u, 3, {0x8B, 0x4A, 0x0C}},                                                             /* table = [o+0xC] */
                    {0x7205D4u, 3, {0x8B, 0x48, 0x10}},                                                             /* table = [o+0x10] */
                    {0x720601u, 18, {0x8B, 0x46, 0x08, 0x89, 0x47, 0x04, 0x8B, 0x4E, 0x0C, 0x89, 0x4F, 0x08, 0x8B, 0x56, 0x10, 0x89, 0x57, 0x0C}},   /* record = entry+8/+C/+10 */
                    {0x726B70u, 27, {0x56, 0x8B, 0x71, 0x6C, 0x33, 0xD2, 0x57, 0x85, 0xF6, 0x74, 0x25, 0x8B, 0x49, 0x78, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0x01, 0x85, 0xC0, 0x74, 0x05, 0x3B, 0x78, 0x04}},
                    /* the title Continue of a 1-1 save: the local record's checkpoints (+8 / +0xC) go to [ebp+0x1C] / [ebp+0x20], then 720870(area [ebp+0x14], A, B) */
                    {0x6D11D5u, 14, {0x8B, 0x4C, 0x24, 0x1C, 0x8B, 0x54, 0x24, 0x20, 0x89, 0x4D, 0x1C, 0x89, 0x55, 0x20}},
                    {0x6D1200u, 19, {0x8B, 0x4D, 0x20, 0x8B, 0x55, 0x1C, 0x8B, 0x45, 0x14, 0x51, 0x8B, 0x0D, 0x40, 0xA3, 0x16, 0x01, 0x52, 0x50, 0xE8}},
                    /* round 2, Continue of a 1-2 save: bit 1 of the checkpoint record's flags (+8) = a section start (1-1 / 1-3: 720870 with the table's checkpoints;
                     * other sections: 720540 + aEpisodeTitle = the section start too); without it the MID-section save goes to 720870(+0x14, +0x1C, +0x20) */
                    {0x6D110Eu, 10, {0x8B, 0x55, 0x08, 0xD1, 0xEA, 0xF6, 0xC2, 0x01, 0x0F, 0x84}},
                    {0x6D111Cu, 17, {0x8B, 0x45, 0x14, 0x3D, 0x00, 0x01, 0x01, 0x91, 0x74, 0x74, 0x3D, 0x00, 0x03, 0x01, 0x91, 0x74, 0x6D}},
                    {0x6D127Bu, 19, {0x8B, 0x45, 0x20, 0x8B, 0x4D, 0x1C, 0x8B, 0x55, 0x14, 0x50, 0x51, 0x8B, 0x0D, 0x40, 0xA3, 0x16, 0x01, 0x52, 0xE8}},
                    /* round 2, difficulty: mCampaignDifficulty's getter 0x746DF0 = [[0x116A7E4]+0x30]; the property name 0x1071968 with that getter at 0x755533 */
                    {0x746E17u, 10, {0x8B, 0x15, 0xE4, 0xA7, 0x16, 0x01, 0x8B, 0x42, 0x30, 0xC3}},
                    {0x755533u, 8, {0xC7, 0x44, 0x24, 0x10, 0x68, 0x19, 0x07, 0x01}},
                    {0x755547u, 8, {0xC7, 0x44, 0x24, 0x1C, 0xF0, 0x6D, 0x74, 0x00}},
                };
                for (unsigned i = 0; i < sizeof code / sizeof code[0]; i++) {
                    uint8_t cur[27];
                    CHECK(fseek(f, (long)(code[i].va - 0x400000u), SEEK_SET) == 0 && fread(cur, 1, code[i].n, f) == code[i].n && !memcmp(cur, code[i].b, code[i].n));
                }
                fclose(f);
                printf("ok   image: the start table code that rb_table_triple mirrors\n");
            }
        }
        /* wp6 live: the bag check before a reload. Two bags as the wp6 host had them: Chris's (slot 1) M92F / G36 / knife / scanner at +4 / +8 / +0x2c / +0x3c,
         * Jill's (slot 0) with only uWpUnarmed; then packcopy 1 -> 0 copies the words +4..+0x140: the four weapons sit in both bags */
        {
            tm_n = 0;
            const uint32_t A = 0x20000u, B = 0x30000u, M92 = 0x51000u, G36 = 0x52000u, KNF = 0x53000u, SCN = 0x54000u, UNARMED = 0x55000u;
            tm_put(A + 4, M92), tm_put(A + 8, G36), tm_put(A + 0x2C, KNF), tm_put(A + 0x3C, SCN), tm_put(A + 0x40, 30u), tm_put(A + 0x44, 0x60000u);   /* ammo 30, a non-weapon object */
            tm_put(B + 4, UNARMED);
            uint32_t sh[8];
            CHECK(rb_bag_shared(tm_rd, wp_obj, NULL, A, B, sh, 8) == 0);   /* clean: nothing shared */
            for (uint32_t off = RB_BAG_FIRST; off < RB_BAG_END; off += 4) tm_put(B + off, tm_rd(NULL, A + off));   /* packcopy 1 -> 0 */
            int n = rb_bag_shared(tm_rd, wp_obj, NULL, A, B, sh, 8);
            CHECK(n == 4 && sh[0] == M92 && sh[1] == G36 && sh[2] == KNF && sh[3] == SCN);   /* the ammo word and the non-weapon object do not count */
            CHECK(rb_bag_shared(tm_rd, wp_obj, NULL, B, A, sh, 2) == 4 && sh[0] == M92 && sh[1] == G36);   /* either order; out holds at most max */
            tm_put(A + 0x50, M92);   /* the same object twice in one bag counts once */
            CHECK(rb_bag_shared(tm_rd, wp_obj, NULL, A, B, NULL, 0) == 4);
            CHECK(rb_bag_shared(tm_rd, wp_obj, NULL, A, A, sh, 8) == 0 && rb_bag_shared(tm_rd, wp_obj, NULL, 0, B, sh, 8) == 0);
            for (uint32_t off = RB_BAG_FIRST; off < RB_BAG_END; off += 4) tm_put(B + off, off == 4 ? UNARMED : 0u);   /* an undo restores Jill's bag */
            CHECK(rb_bag_shared(tm_rd, wp_obj, NULL, A, B, sh, 8) == 0);
        }
        sim_start();
        rb_triple rec = {0x91010100u, 7, 9}, out;
        /* not the host, no session, the section change: no start */
        CHECK(rb_start_seen(&P[1].b, 0x83E89Bu) == 0 && !P[1].b.st_pending);
        S.started = 0;
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 0 && !P[0].b.st_pending);
        S.started = 1;
        unsigned secn = P[0].b.st_section;
        CHECK(rb_start_seen(&P[0].b, 0x42D47Cu) == 0 && P[0].b.st_section == secn + 1 && !P[0].b.st_pending);
        CHECK(rb_start_triple(&P[0].b, &rec) == RB_ST_NONE);   /* nothing pending */
        /* the title's New Game: sent, the client gets it as a host triple */
        unsigned sent = P[0].b.rs_sent;
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1 && P[0].b.st_pending);
        CHECK(rb_start_triple(&P[0].b, &rec) == RB_ST_SENT && P[0].b.rs_sent == sent + 1 && !P[0].b.st_pending);
        tick(30);
        CHECK(P[1].b.rs_have && P[1].b.rs_area == 0x91010100u && P[1].b.rs_a == 7 && P[1].b.rs_b == 9);
        /* the follow at the title has no upper age limit (a late guest), the in-level one keeps the 30 s */
        tick(40000);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 1) == 1 && out.area == 0x91010100u && out.a == 7 && out.b == 9);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 1) == 0);   /* once per triple */
        rb_triple t = out;
        rb_restart_client(&P[1].b, &t);
        /* round 2: the host's difficulty travels with its restart; the guest's rewrite hands it to the glue (which writes [[0x116A7E4]+0x30] before the reload) */
        {
            uint8_t m[20];
            CHECK(rb_msg_restart_d(m, sizeof m, 7, 0x91010100u, 256, 1, 2) == 17 && m[16] == 2);
            CHECK(rb_msg_restart_d(m, sizeof m, 7, 0x91010100u, 256, 1, -1) == 16 && rb_msg_restart_d(m, sizeof m, 7, 0x91010100u, 256, 1, 9) == 16);
            CHECK(rb_msg_restart_d(m, 16, 7, 0x91010100u, 256, 1, 2) == 0);
            P[0].b.tx_diff = 2;
            CHECK(rb_restart_host(&P[0].b, &rec) == 1);
            tick(30);
            CHECK(P[1].b.rs_have && P[1].b.rs_diff == 2);
            rb_triple own = {0x91010100u, 5, 5};
            CHECK(rb_restart_client(&P[1].b, &own) == 1 && P[1].b.applied_diff == 2 && own.a == 7 && own.b == 9);
            CHECK(rb_restart_client(&P[1].b, &own) == 0 && P[1].b.applied_diff == -1);   /* no triple waiting: the own checkpoint, nothing to take */
            P[0].b.tx_diff = -1;   /* the host could not read its difficulty: the old 16-byte message, the guest keeps its own */
            CHECK(rb_restart_host(&P[0].b, &rec) == 1);
            tick(30);
            own = (rb_triple){0x91010100u, 5, 5};
            CHECK(P[1].b.rs_have && P[1].b.rs_diff == -1 && rb_restart_client(&P[1].b, &own) == 1 && P[1].b.applied_diff == -1);
            int L = rb_msg_restart_d(m, sizeof m, 99, 0x91010200u, 40, 3, 1);
            m[16] = 7;   /* out of range: the triple counts, the difficulty does not */
            CHECK(L == 17 && rb_on_event(&P[1].b, m, 17) == 1 && P[1].b.rs_diff == -1 && P[1].b.rs_area == 0x91010200u);
            rb_reset(&P[1].b);
            CHECK(!P[1].b.rs_have && P[1].b.rs_diff == -1 && P[1].b.applied_diff == -1);
            /* round 2 rule: Continue of a 1-2 save where the host is at a LATER point than the guest. The host's mid-section Continue is FUN_00720870(the save's
             * area, A, B) (0x6D127B..0x6D128D) = its restart hook: the guest follows the HOST's checkpoint (at the title, by itself), never its own */
            tick(3500);
            rb_triple h12 = {0x91010200u, 40, 3}, g12 = {0x91010200u, 10, 1};
            P[0].b.tx_diff = 1;
            CHECK(rb_restart_host(&P[0].b, &h12) == 1);
            tick(1600);
            CHECK(rb_restart_follow_at(&P[1].b, &out, 1) == 1 && out.area == 0x91010200u && out.a == 40 && out.b == 3);
            CHECK(rb_restart_client(&P[1].b, &g12) == 1 && g12.area == 0x91010200u && g12.a == 40 && g12.b == 3 && P[1].b.applied_diff == 1);
            P[0].b.tx_diff = -1;
            /* rev1 live 14 (m14d, LIVE): the host's title Continue of a 1-3 save that the guest's save holds too: the follow at the title by the direct
             * FUN_00720870 never read the guest's save, so its own human had NO weapon (`bag C ... +30=uWpUnarmed +3c=uWpScanner`). With titlestock the follow
             * goes through the guest's own stock Continue (aStartContinue -> FUN_006D0FF0 reads its save = its bag), the restart hook still takes the host's triple */
            for (int ts = 0; ts <= 1; ts++) {
                tick(3500);
                rb_triple h13 = {0x91010300u, 256, 2};
                P[1].b.cfg.title_stock = ts;
                P[0].b.tx_cp.have = 1, P[0].b.tx_cp.id = 1, P[0].b.tx_cp.mask = 3;   /* a mid-section record (mID 1): the guest's save holds the same one */
                CHECK(rb_restart_host(&P[0].b, &h13) == 1);
                tick(30);
                P[1].b.cp_saved = (rb_cprec){P[1].b.rs_cp.have, P[1].b.rs_cp.id, P[1].b.rs_cp.mask, P[1].b.rs_area, P[1].b.rs_a, P[1].b.rs_b};   /* the guest's save = the host's checkpoint */
                tick(1600);
                CHECK(rb_restart_follow_at(&P[1].b, &out, 1) == 1 && out.area == 0x91010300u && out.a == 256 && out.b == 2);
                if (!ts) CHECK(!P[1].b.fol_try_stock);   /* before: the direct call at the title (the m14d empty bag) */
                else CHECK(P[1].b.fol_try_stock);        /* titlestock: through the guest's own stock Continue (its save, its bag) */
            }
            /* rev1 live 14 (m14h, LIVE): at the title the guest's engine has written no checkpoint yet (cp_saved.have 0; its record reads area 91ff0000), the
             * host's title Continue of a section-start save sends mID -1 mask 0: before the fix saved_ok was 0 there, titlestock never acted (`no own Continue
             * used the host's triple within 1500 ms: the client restarts by itself` without "through its own stock Continue", the guest's Jill unarmed).
             * Now an unknown save counts at the title with titlestock; a KNOWN other save still goes direct; without titlestock nothing changes */
            for (int k = 0; k < 4; k++) {
                tick(3500);
                rb_triple h13 = {0x91010300u, 256, 2};
                P[1].b.cfg.title_stock = k != 1;
                P[0].b.tx_cp.have = 1, P[0].b.tx_cp.id = -1, P[0].b.tx_cp.mask = 0;
                CHECK(rb_restart_host(&P[0].b, &h13) == 1);
                tick(30);
                if (k == 2) P[1].b.cp_saved = (rb_cprec){1, 2, 7, 0x91010100u, 256, 4};   /* the guest's save is known and holds a 1-1 checkpoint */
                else if (k == 3) P[1].b.cp_saved = (rb_cprec){1, -1, 0, 0x91FF0000u, -1, -1};   /* m14i LIVE: the title's CLEARED record taken as cp_saved */
                else memset(&P[1].b.cp_saved, 0, sizeof P[1].b.cp_saved);                /* nothing known at the title */
                tick(1600);
                CHECK(rb_restart_follow_at(&P[1].b, &out, 1) == 1 && out.area == 0x91010300u && out.a == 256 && out.b == 2);
                if (k == 0 || k == 3) CHECK(P[1].b.fol_try_stock);   /* titlestock, unknown / cleared record: its own stock Continue (r45 / r46) */
                else CHECK(!P[1].b.fol_try_stock);          /* k 1: no titlestock = the direct call; k 2: a known other save = the direct call */
            }
            memset(&P[1].b.cp_saved, 0, sizeof P[1].b.cp_saved);
            P[1].b.cfg.title_stock = 0;
            P[0].b.tx_cp.have = 0;
        }
        /* the title Continue of a 1-1 save: the restart hook sends first, the record is the same a frame later: one RESTART only */
        sent = P[0].b.rs_sent;
        CHECK(rb_start_seen(&P[0].b, 0x6D11D5u) == 1);
        CHECK(rb_restart_host(&P[0].b, &rec) == 1);
        CHECK(rb_start_triple(&P[0].b, &rec) == RB_ST_DUP && P[0].b.rs_sent == sent + 1 && P[0].b.st_dup >= 1);
        tick(3500);   /* the same triple later is a new start (a Continue after a death at the same checkpoint) */
        CHECK(rb_start_seen(&P[0].b, 0x6D0DEEu) == 1 && rb_start_triple(&P[0].b, &rec) == RB_ST_SENT);
        /* a bad record is not sent */
        rb_triple bad = {0x12345678u, 0, 0};
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1 && rb_start_triple(&P[0].b, &bad) == RB_ST_BAD && rb_start_triple(&P[0].b, NULL) == RB_ST_NONE);
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1 && rb_start_triple(&P[0].b, NULL) == RB_ST_BAD);
        /* allep1: a start / restart in any paired section (here 3-1) is sent; the solo part and the ids that are no section are refused (section 8a) */
        rb_triple ep3 = {0x91030100u, 4, 5};
        sent = P[0].b.rs_sent;
        CHECK(rb_start_seen(&P[0].b, 0x6D1188u) == 1 && rb_start_triple(&P[0].b, &ep3) == RB_ST_SENT && P[0].b.rs_sent == sent + 1);
        tick(3500);
        CHECK(rb_start_seen(&P[0].b, 0x6D1188u) == 1 && rb_start_triple(&P[0].b, &rec) == RB_ST_SENT);
        /* a reset forgets a pending start */
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1);
        rb_reset(&P[0].b);
        CHECK(!P[0].b.st_pending && !P[0].b.tx_have);
        char st[2400];
        rb_status(&P[0].b, st, sizeof st);
        CHECK(strstr(st, "story starts (FUN_00720540)") != NULL);
    }

    /* ============================================ 7. ep1-flow E4: the section change (the host's next section; a client that did not reach the end follows) */
    {
        /* the build's code that the glue relies on (image base 0x400000): cEpisodeEndState_vf7 reads the finished section as [[arg+4]]->vf+0x10 (DTI) +0x1C (0x42D3CB,
         * the glue's cb_epend reads the same); the next-episode branch (the end of 1-3) calls FUN_00720540 at 0x42D4AD and goes to aEpisodeEnd 0x116B208; the end of 1-1
         * goes to aEpisodeTitle 0x116B228 (0x42D4ED); cEpisodeEndFinalState's update goes to aEpisodeTitle (0x437FF1) after the result / save flow */
        const char *path = getenv("REV1_IMAGE");
        FILE *f = fopen(path && *path ? path : "../../image/rerev-image.bin", "rb");
        if (f) {
            static const uint8_t rd_from[13] = {0x8B, 0x4D, 0x04, 0x8B, 0x01, 0x8B, 0x50, 0x10, 0xFF, 0xD2, 0x8B, 0x40, 0x1C};
            static const uint8_t ep_end[12] = {0x56, 0xE8, 0x8E, 0x30, 0x2F, 0x00, 0xC7, 0x47, 0x04, 0x08, 0xB2, 0x16};
            static const uint8_t to_title[12] = {0xE8, 0x53, 0x30, 0x2F, 0x00, 0xC7, 0x47, 0x04, 0x28, 0xB2, 0x16, 0x01};
            static const uint8_t fin_title[8] = {0xC7, 0x44, 0x24, 0x04, 0x28, 0xB2, 0x16, 0x01};
            uint8_t c[16];
            CHECK(fseek(f, 0x42D3CBL - 0x400000L, SEEK_SET) == 0 && fread(c, 1, 13, f) == 13 && !memcmp(c, rd_from, 13));
            CHECK(fseek(f, 0x42D4ACL - 0x400000L, SEEK_SET) == 0 && fread(c, 1, 12, f) == 12 && !memcmp(c, ep_end, 12));
            CHECK(0x42D4ADu + 5u + 0x002F308Eu == 0x720540u && 0x42D4B2u >= RB_SECTION_FN_LO && 0x42D4B2u < RB_SECTION_FN_HI);
            CHECK(fseek(f, 0x42D4E8L - 0x400000L, SEEK_SET) == 0 && fread(c, 1, 12, f) == 12 && !memcmp(c, to_title, 12));
            CHECK(fseek(f, 0x437FF1L - 0x400000L, SEEK_SET) == 0 && fread(c, 1, 8, f) == 8 && !memcmp(c, fin_title, 8));
            fclose(f);
            printf("ok   image: vf7's finished-section read, the end-of-episode branch (aEpisodeEnd), the 1-1 branch and the Final state (aEpisodeTitle)\n");
        } else
            printf("(no executable image: the episode-end code is not compared with the build)\n");
    }
    {
        const rb_triple s12 = {0x91010200u, 512, 0}, s13 = {0x91010300u, 256, 2};
        rb_triple out, own;
        uint8_t m[RB_MSG_MAX];
        /* the message */
        CHECK(rb_msg_section(m, sizeof m, 7, &s12, 0x91010100u, RB_SEC_TIMEOUT) == RB_SECTION_MSG_LEN && RB_SECTION_MSG_LEN <= RB_MSG_MAX && RB_SECTION_MSG_LEN <= CP_EVENT_MAX);
        CHECK(m[0] == RB_MSG_VER && m[1] == RBM_SECTION && m[20] == RB_SEC_TIMEOUT && rb_msg_section(m, 20, 7, &s12, 0, 0) == 0 && rb_msg_section(m, sizeof m, 7, NULL, 0, 0) == 0);
        CHECK(rb_msg_epdone(m, sizeof m, 0x91010300u) == RB_EPDONE_MSG_LEN && m[1] == RBM_EPDONE && rb_msg_epdone(m, 5, 1) == 0);

        /* (a) both PCs reach the end of 1-1: the rendezvous, then each changes the section by itself; the host's triple is consumed by the client's own change */
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD);
        tick(30);
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);   /* the host was there first */
        tick(30);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED && P[0].b.pt[RB_SECTION].st == 2);
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT && P[0].b.sec_sent == 1);
        CHECK(strstr(logbuf, "host section change 91010100 -> 91010200 (section end released by the other PC): SECTION #1 sent, the client's start 512 / 0"));
        CHECK(rb_section_change(&P[1].b, 0x91010100u, 0x91010200u, NULL) == RB_SC_NOTED);   /* the client's own change: a moment before the triple arrives */
        tick(30);
        CHECK(P[1].b.sec_got == 1 && P[1].b.sec_consumed == 1 && !P[1].b.rs_have && !P[1].b.sec_pending);
        CHECK(strstr(logbuf, "this PC changed to that section") != NULL);
        tick(10000);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0 && P[1].b.sec_followed == 0);
        /* (a') the triple arrives first while the client is held at its own end: no follow, its own change consumes it */
        sim_start();
        S.phase = 3;
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_HOLD);   /* the client first this time */
        tick(30);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED && P[0].b.pt[RB_SECTION].st == 2);
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT);
        tick(30);   /* the host's ARRIVE and its SECTION are both on the client now */
        CHECK(P[1].b.sec_pending == 1 && P[1].b.rs_have == 1);
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);   /* released by the host's arrival */
        tick(5000);   /* its own FUN_00720540 is late (simulated): its section end ran just now, so the follow waits for its own change */
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0 && P[1].b.sec_pending == 1);
        CHECK(rb_section_change(&P[1].b, 0x91010100u, 0x91010200u, NULL) == RB_SC_NOTED && !P[1].b.sec_pending && !P[1].b.rs_have && P[1].b.sec_consumed == 1);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);
        /* (a'') the same, but its own change never comes: after sec_wait_ms the client follows anyway */
        sim_start();
        S.phase = 3;
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_HOLD);
        tick(30);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED && rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT);
        tick(30);
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);
        tick(29000);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);
        tick(1100);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 1 && out.area == 0x91010200u && P[1].b.sec_followed == 1);

        /* (b) the client never reaches the end: the host's 180 s cap, its SECTION says TIMEOUT, the client follows after sec_follow_ms with the start triple */
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD);
        while (rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD) tick(50);
        CHECK(P[0].b.pt[RB_SECTION].st == 3 && P[0].b.k_timeout[RB_SECTION] == 1);
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT && strstr(logbuf, "released by its TIMEOUT"));
        tick(30);
        CHECK(P[1].b.sec_pending == 1 && P[1].b.sec_how == RB_SEC_TIMEOUT && P[1].b.sec_from == 0x91010100u);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);   /* too young */
        tick(3900);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);
        tick(200);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 1 && out.area == 0x91010200u && out.a == 512 && out.b == 0 && P[1].b.sec_followed == 1);
        CHECK(strstr(logbuf, "it follows the host from 91010100 into 91010200 (start 512 / 0)") && strstr(logbuf, "it did not reach the end"));
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);   /* once */
        own = (rb_triple){0x91010100u, 256, 5};   /* the follow's FUN_00720870 call passes the restart hook: the triple is the host's next section */
        CHECK(rb_restart_client(&P[1].b, &own) == 1 && own.area == 0x91010200u && own.a == 512 && own.b == 0 && !P[1].b.sec_pending && !P[1].b.rs_have);
        CHECK(strstr(logbuf, "-> host's next section (area 91010200, 512, 0)") != NULL);
        /* (b') a section triple is not dropped by the 30 s freshness of a Continue triple: an own Continue 40 s later still goes into the next section */
        sim_start();
        S.phase = 3;
        CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT);   /* (no rendezvous ran: "no rendezvous ran") */
        tick(30);
        CHECK(P[1].b.sec_pending && P[1].b.sec_how == RB_SEC_ALONE);
        P[1].b.cfg.sec_follow_ms = 0x7FFFFFFF;   /* the follow switched off: only the own Continue is left */
        tick(40000);
        own = (rb_triple){0x91010200u, 512, 0};
        CHECK(rb_restart_client(&P[1].b, &own) == 1 && own.area == 0x91010300u && own.a == 256 && own.b == 2);

        /* (c) the client went on alone (its own timeout) and the host reaches the end minutes later: the old own change does not consume, the client reloads with the host */
        sim_start();
        S.phase = 3;
        CHECK(rb_section_change(&P[1].b, 0x91010100u, 0x91010200u, NULL) == RB_SC_NOTED);
        tick(300000);
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT);
        tick(30);
        CHECK(P[1].b.sec_pending == 1 && P[1].b.sec_consumed == 0);
        tick(4100);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 1 && out.area == 0x91010200u);
        /* (c') the own change to ANOTHER area than the host's (a divergence) does not consume either */
        sim_start();
        S.phase = 3;
        CHECK(rb_section_change(&P[1].b, 0x91010200u, 0x91010300u, NULL) == RB_SC_NOTED);
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT);
        tick(4200);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == RB_FOL_CALL && out.area == 0x91010200u);   /* (rev1 live 10) its level is not the section the host left: the direct call */

        /* (d) refusals: no start triple on the host, no session, a lab session outside Episode 1, a SECTION to the host / of another episode / with a bad triple */
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, NULL) == RB_SC_BAD && strstr(logbuf, "NO start triple for the client") && P[0].b.sec_sent == 0);
        rb_triple wrong = {0x91010300u, 256, 2};
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &wrong) == RB_SC_BAD);   /* a triple of another area */
        CHECK(rb_section_change(&P[0].b, 0x910F0100u, 0x91010200u, &s12) == RB_SC_NONE);   /* a lab warp into a part that is no paired section (F1): each PC goes on by itself */
        CHECK(rb_section_change(&P[0].b, 0, 0x91010200u, &s12) == RB_SC_SENT);             /* an unreadable section is taken as a paired one */
        S.started = 0;
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_NONE);
        S.started = 1;
        nq = 0;
        rb_msg_section(m, sizeof m, 9, &s12, 0x91010100u, RB_SEC_PEER);
        CHECK(rb_on_event(&P[0].b, m, RB_SECTION_MSG_LEN) == 0 && strstr(logbuf, "SECTION from the client ignored"));
        rb_triple other = {0x910F0100u, 256, 1}, zero = {0x91010200u, 0, 0};
        rb_msg_section(m, sizeof m, 10, &other, 0x91010300u, RB_SEC_PEER);
        CHECK(rb_on_event(&P[1].b, m, RB_SECTION_MSG_LEN) == 0 && strstr(logbuf, "not a section of the campaign") && !P[1].b.sec_pending);
        rb_msg_section(m, sizeof m, 11, &zero, 0x91010100u, RB_SEC_PEER);
        CHECK(rb_on_event(&P[1].b, m, RB_SECTION_MSG_LEN) == 0 && !P[1].b.sec_pending && P[1].b.sec_bad == 2);
        CHECK(rb_on_event(&P[1].b, m, RB_SECTION_MSG_LEN - 1) == 0);
        /* (e) a host Continue after the SECTION is newer: the waiting triple is the Continue's (the regular 1.5 s follow) */
        rb_msg_section(m, sizeof m, 12, &s13, 0x91010200u, RB_SEC_PEER);
        CHECK(rb_on_event(&P[1].b, m, RB_SECTION_MSG_LEN) == 1 && P[1].b.sec_pending);
        rb_triple cont = {0x91010200u, 512, 0};
        CHECK(rb_restart_host(&P[0].b, &cont) == 1);
        tick(30);
        CHECK(!P[1].b.sec_pending && P[1].b.rs_area == 0x91010200u);
        tick(1600);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 1 && out.area == 0x91010200u && out.a == 512);
        char st[2600];
        rb_status(&P[1].b, st, sizeof st);
        CHECK(strstr(st, "ep1-flow: own section changes") && strstr(st, "end of co-op: here 0, partner 0"));
    }

    /* ============================================ 8. ep1-flow E5 / allep1: the end of co-op (12-3 -> the end of the campaign): EPDONE, no follow after it, the close after the stock end flow */
    {
        rb_triple out;
        const rb_triple s13 = {0x91010300u, 256, 2};
        /* (a) both reach the end of 1-3 */
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD);
        tick(30);
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);
        tick(30);
        CHECK(rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED);
        CHECK(rb_section_change(&P[0].b, 0x910C0300u, 0x91FF0000u, NULL) == RB_SC_EPEND && P[0].b.ep_local && P[0].b.ep_sent == 1 && P[0].b.sec_sent == 0);
        CHECK(strstr(logbuf, "END OF CO-OP on this PC (host, section end released by the other PC): 910C0300 -> 91FF0000 (the campaign has ended); the partner is told (EPDONE)"));
        CHECK(rb_section_change(&P[1].b, 0x910C0300u, 0x91FF0000u, NULL) == RB_SC_EPEND && P[1].b.ep_local);
        tick(30);
        int loc = -1;
        CHECK(rb_ep_known(&P[0].b, &loc) == 1 && loc == 1 && P[0].b.ep_peer && P[1].b.ep_peer && P[0].b.ep_got == 1 && P[1].b.ep_got == 1);
        CHECK(strstr(logbuf, "EPDONE from the client: it passed the end of co-op (left 910C0300); this PC passed it too"));
        /* the close: not while the old section or the unknown area runs, not during the end flow, once when the end flow is over */
        uint32_t t0 = T;
        CHECK(rb_ep_close_step(&P[1].b, 0, 0, T) == 0);    /* aEpisode13 still (the state change of vf7 comes) */
        CHECK(rb_ep_close_step(&P[1].b, -1, 0, T) == 0);   /* the area is not readable for a frame */
        tick(500);
        CHECK(rb_ep_close_step(&P[1].b, 1, 0, T) == 0 && P[1].b.ep_seen_flow == 1 && strstr(logbuf, "the stock end flow runs on this PC (aEpisodeEnd: result, save)"));
        tick(60000);
        CHECK(rb_ep_close_step(&P[1].b, 1, 0, T) == 0 && rb_ep_close_step(&P[1].b, -1, 0, T) == 0);   /* result, save; a frame without a readable area changes nothing */
        CHECK(rb_ep_close_step(&P[1].b, 0, 0, T) == 1 && P[1].b.ep_closed && strstr(logbuf, "the session closes now (the stock end flow (result, save) is over"));
        CHECK(rb_ep_close_step(&P[1].b, 0, 1, T) == 0);    /* once */
        (void)t0;
        /* no follow, no Continue triple, no section triple after the end */
        rb_triple cont = {0x91010300u, 257, 1};
        CHECK(rb_restart_host(&P[0].b, &cont) == 1);   /* (the host's engine would not do this; the client ignores it anyway) */
        tick(30);
        CHECK(!P[1].b.rs_have && P[1].b.rs_rejected >= 1 && strstr(logbuf, "ignored: the co-op session has reached its end"));
        uint8_t m[RB_MSG_MAX];
        rb_msg_section(m, sizeof m, 5, &s13, 0x91010200u, RB_SEC_PEER);
        CHECK(rb_on_event(&P[1].b, m, RB_SECTION_MSG_LEN) == 0 && !P[1].b.sec_pending);
        tick(5000);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0 && rb_restart_follow_at(&P[1].b, &out, 1) == 0);
        /* (b) the latest point: the end flow was never recognised, a level that is no paired section is there (2-1 loaded) */
        sim_start();
        S.phase = 3;
        CHECK(rb_section_change(&P[0].b, 0x910C0300u, 0x91FF0000u, NULL) == RB_SC_EPEND);
        CHECK(rb_ep_close_step(&P[0].b, 0, 1, T) == 0);   /* within 2 s of the end: not yet (the old level may still be listed) */
        tick(2100);
        CHECK(rb_ep_close_step(&P[0].b, 0, 1, T) == 1 && strstr(P[0].b.why, "a level that is no paired section"));
        /* (c) the cap */
        sim_start();
        S.phase = 3;
        CHECK(rb_section_change(&P[0].b, 0x910C0300u, 0x91FF0000u, NULL) == RB_SC_EPEND);
        tick(1000);
        CHECK(rb_ep_close_step(&P[0].b, 0, 0, T) == 0);
        P[0].b.cfg.ep_close_cap_ms = 5000;
        tick(4100);
        CHECK(rb_ep_close_step(&P[0].b, 0, 0, T) == 1 && strstr(P[0].b.why, "the time limit"));
        /* (d) the client is behind: the host's EPDONE drops a waiting triple and stops the follow; the client has no close of its own (the host's close ends it) */
        sim_start();
        S.phase = 3;
        logbuf[0] = 0;
        CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT);
        tick(30);
        CHECK(P[1].b.sec_pending);
        CHECK(rb_section_change(&P[0].b, 0x910C0300u, 0x91FF0000u, NULL) == RB_SC_EPEND);
        tick(30);
        CHECK(P[1].b.ep_peer && !P[1].b.ep_local && !P[1].b.sec_pending && !P[1].b.rs_have);
        CHECK(strstr(logbuf, "EPDONE from the host: it passed the end of co-op (left 910C0300): this PC has not"));
        loc = -1;
        CHECK(rb_ep_known(&P[1].b, &loc) == 1 && loc == 0);
        tick(5000);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0 && rb_ep_close_step(&P[1].b, 0, 1, T) == 0);
        /* (e) a lab warp into a part that is no paired section never ends co-op; a reset forgets everything */
        sim_start();
        S.phase = 3;
        CHECK(rb_section_change(&P[0].b, 0x910F0100u, 0x91010200u, NULL) == RB_SC_NONE && !P[0].b.ep_local);
        CHECK(rb_section_change(&P[0].b, 0x910C0300u, 0x91FF0000u, NULL) == RB_SC_EPEND);
        rb_reset(&P[0].b);
        CHECK(!P[0].b.ep_local && !P[0].b.ep_peer && !P[0].b.ep_closed && !P[0].b.own_sec_have && rb_ep_known(&P[0].b, NULL) == 0);
        CHECK(rb_ep_close_step(&P[0].b, 0, 1, T + 100000) == 0);
    }

    /* ============================================ 8a. allep1: every paired section of every episode is shared (no setting) */
    {
        /* (a) the id table: the 29 story sections (28 paired + the solo 16-2), the ids that are no section, the end */
        static const uint32_t pairs[28] = {0x91010100u, 0x91010200u, 0x91010300u, 0x91020100u, 0x91020200u, 0x91030100u, 0x91030200u, 0x91040100u, 0x91050100u, 0x91050200u,
                                           0x91050300u, 0x91060100u, 0x91060200u, 0x91070100u, 0x91070200u, 0x91080100u, 0x91080200u, 0x91080300u, 0x91090100u, 0x91090200u,
                                           0x910A0100u, 0x910A0200u, 0x910B0100u, 0x910B0200u, 0x910C0100u, 0x910C0200u, 0x910C0300u, 0x91100100u};
        for (int i = 0; i < 28; i++) CHECK(rb_section_kind(pairs[i]) == RB_SK_PAIR);
        CHECK(rb_section_kind(0x91100200u) == RB_SK_SOLO);
        CHECK(rb_section_kind(0x91000100u) == RB_SK_NOTSEC && rb_section_kind(0x91000200u) == RB_SK_NOTSEC && rb_section_kind(0x91080400u) == RB_SK_NOTSEC && rb_section_kind(0x910F0100u) == RB_SK_NOTSEC);
        CHECK(rb_section_kind(0x91010400u) == RB_SK_NOTSEC && rb_section_kind(0x91130100u) == RB_SK_NOTSEC && rb_section_kind(0x91010101u) == RB_SK_NOTSEC);   /* any other 0x91 id */
        CHECK(rb_section_kind(0x91FF0000u) == RB_SK_END && rb_section_kind(0x92010100u) == RB_SK_END && rb_section_kind(0) == RB_SK_END);
        int npair = 0, nsolo = 0;
        for (uint32_t ep = 0; ep < 0x20; ep++)
            for (uint32_t sec = 0; sec < 0x10; sec++) {
                int k = rb_section_kind(0x91000000u | (ep << 16) | (sec << 8));
                npair += k == RB_SK_PAIR, nsolo += k == RB_SK_SOLO;
            }
        CHECK(npair == 28 && nsolo == 1);   /* the catalog: 29 sections, 28 of them paired */
        CHECK(sec_kind(0x91020100u) == rb_section_kind(0x91020100u));   /* the lobby's copy (secids1.h) is the same function */
        CHECK(sec_spectate(0x91100200u) == 1 && sec_spectate(0x91020200u) == 2 && sec_spectate(0x91100100u) == 2 && sec_spectate(0x91020100u) == 0 && sec_spectate(0x910C0300u) == 0);   /* step 3: where the second human spectates */
        CHECK(rb_end_kind_from(0x910C0300u) == RB_EK_CAMPAIGN && rb_end_kind_from(0x91100200u) == RB_EK_CAMPAIGN && rb_end_kind_from(0x91010300u) == RB_EK_NOTSEC);

        /* (b) rb_area_ok: every paired section of any episode, nothing else */
        CHECK(rb_area_ok(0x91010200u) && rb_area_ok(0x91020100u) && rb_area_ok(0x91100100u) && rb_area_ok(0x910C0300u));
        CHECK(rb_area_ok(0x91100200u) && !rb_area_ok(0x91080400u) && !rb_area_ok(0x91FF0000u) && !rb_area_ok(0x91000100u));
        CHECK(!rb_area_ok(0x910E0000u) && rb_section_kind(0x910E0000u) != RB_SK_PAIR && rb_end_kind_from(0x910C0300u) == RB_EK_CAMPAIGN);   /* allep4s: the area after 12-3 is 0x910E0000 (no start table entry): END OF CO-OP, not a section to follow */

        /* (c) a section change across an episode boundary is a normal SECTION change, no EPDONE, no close; the client follows it */
        static const struct { uint32_t from, to; int32_t a, b; } hops[] = {
            {0x91010300u, 0x91020100u, 256, 1}, {0x91020200u, 0x91030100u, 256, 1}, {0x91030200u, 0x91040100u, 256, 1}, {0x91050300u, 0x91060100u, 256, 1},
            {0x910C0200u, 0x910C0300u, 256, 1}};
        for (unsigned h = 0; h < sizeof hops / sizeof hops[0]; h++) {
            sim_start();
            S.phase = 3;
            rb_triple nx = {hops[h].to, hops[h].a, hops[h].b};
            logbuf[0] = 0;
            CHECK(rb_section_change(&P[0].b, hops[h].from, hops[h].to, &nx) == RB_SC_SENT && !P[0].b.ep_local && P[0].b.ep_sent == 0 && P[0].b.sec_sent == 1);
            tick(30);
            CHECK(P[1].b.sec_pending && P[1].b.rs_area == hops[h].to && !P[1].b.ep_peer && P[1].b.sec_bad == 0);
            CHECK(rb_section_change(&P[1].b, hops[h].from, hops[h].to, NULL) == RB_SC_NOTED && !P[1].b.ep_local && !P[1].b.sec_pending && P[1].b.sec_consumed == 1);   /* its own stock change consumed it */
            CHECK(rb_ep_close_step(&P[0].b, 1, 1, T + 100000) == 0 && rb_ep_close_step(&P[1].b, 1, 1, T + 100000) == 0);   /* nothing to close */
            CHECK(!strstr(logbuf, "END OF"));
        }
        {   /* the client that is behind follows the next episode's first section with the host's start triple (the stock section end, then the call) */
            sim_start();
            S.phase = 3;
            rb_triple nx = {0x91020100u, 256, 1}, out;
            CHECK(rb_section_change(&P[0].b, 0x91010300u, 0x91020100u, &nx) == RB_SC_SENT);
            tick(4200);
            CHECK(rb_restart_follow_at(&P[1].b, &out, 0) != 0 && out.area == 0x91020100u && out.a == 256 && out.b == 1 && P[1].b.sec_followed == 1);
        }
        /* (d) the client rejects a SECTION into the solo part / a non-section / the end; with the option off it still rejects every section outside Episode 1 */
        {
            uint8_t m[RB_MSG_MAX];
            static const uint32_t bad[] = {0x91000100u, 0x91080400u, 0x910F0100u};
            for (unsigned i = 0; i < 3; i++) {
                sim_start();
                S.phase = 3;
                    logbuf[0] = 0;
                rb_triple t = {bad[i], 256, 1};
                rb_msg_section(m, sizeof m, 20, &t, 0x91010100u, RB_SEC_PEER);
                CHECK(rb_on_event(&P[1].b, m, RB_SECTION_MSG_LEN) == 0 && !P[1].b.sec_pending && P[1].b.sec_bad == 1);
                CHECK(strstr(logbuf, "not a section of the campaign"));
            }
            sim_start();
            S.phase = 3;
            rb_triple t2 = {0x91050100u, 256, 1};
            rb_msg_section(m, sizeof m, 21, &t2, 0x91040100u, RB_SEC_PEER);
            CHECK(rb_on_event(&P[1].b, m, RB_SECTION_MSG_LEN) == 1 && P[1].b.sec_pending && P[1].b.rs_area == 0x91050100u && P[1].b.sec_bad == 0);   /* 5-1 is followed */
        }

        /* (e) the host's story start / restart: paired sections of any episode go to the client, the solo part and the non-sections end the session (not sent) */
        {
            sim_start();
            static const struct { uint32_t area; int want; } cases[] = {{0x91020100u, RB_ST_SENT}, {0x91030100u, RB_ST_SENT}, {0x91050100u, RB_ST_SENT}, {0x91100100u, RB_ST_SENT},
                                                                         {0x91100200u, RB_ST_SENT}, {0x91000100u, RB_ST_NOTSEC}, {0x91000200u, RB_ST_NOTSEC},
                                                                         {0x91080400u, RB_ST_NOTSEC}, {0x910F0100u, RB_ST_NOTSEC}};
            for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
                rb_triple rec = {cases[i].area, 256, 1};
                unsigned sent = P[0].b.rs_sent;
                logbuf[0] = 0;
                tick(4000);   /* (a start of the same triple within 3 s would be a duplicate) */
                CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1);
                int r = rb_start_triple(&P[0].b, &rec);
                CHECK(r == cases[i].want);
                CHECK(P[0].b.rs_sent == sent + (cases[i].want == RB_ST_SENT));
                if (cases[i].want == RB_ST_NOTSEC) CHECK(strstr(logbuf, "refused for the session: not a section of the campaign"));
                CHECK(!P[0].b.st_pending);
            }
            CHECK(P[0].b.st_secbad == 4);
            tick(30);
            CHECK(P[1].b.rs_have && P[1].b.rs_area == 0x91100200u);   /* the last one (the solo 16-2, spectated) reached the client */
            /* the restart hook (a Continue / game-over Yes): the same rule */
            rb_triple r1 = {0x91100200u, 256, 1}, r2 = {0x91040100u, 256, 1}, r3 = {0x91000100u, 1, 1};
            CHECK(rb_restart_host(&P[0].b, &r1) == 1 && rb_restart_host(&P[0].b, &r3) == RB_ST_NOTSEC);   /* the solo 16-2 is followed (the second human spectates) */
            tick(4000);
            CHECK(rb_restart_host(&P[0].b, &r2) == 1);
            /* no session: nothing is refused (the game plays alone) */
            S.started = 0;
            CHECK(rb_restart_host(&P[0].b, &r1) == 0);
            S.started = 1;
            /* a junk start record (0x92 START id, the title's New Game before the table fallback) is still BAD, not a refusal */
            rb_triple junk = {0x92010100u, 256, 1};
            CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1 && rb_start_triple(&P[0].b, &junk) == RB_ST_BAD);
        }

        /* (f) the end of co-op: the campaign over (12-3 -> 0x91FF0000), the solo part (16-1 -> 16-2); kinds on both PCs, the close rule */
        {
            sim_start();
            S.phase = 3;
            logbuf[0] = 0;
            CHECK(rb_section_change(&P[0].b, 0x910C0300u, 0x91FF0000u, NULL) == RB_SC_EPEND && P[0].b.ep_local && P[0].b.ep_kind == RB_EK_CAMPAIGN && P[0].b.ep_sent == 1);
            CHECK(strstr(logbuf, "END OF CO-OP on this PC (host, section end no rendezvous ran): 910C0300 -> 91FF0000 (the campaign has ended)"));
            tick(30);
            CHECK(P[1].b.ep_peer && !P[1].b.ep_local && P[1].b.ep_kind == RB_EK_CAMPAIGN && strstr(logbuf, "EPDONE from the host: it passed the end of co-op (left 910C0300): this PC has not"));
            CHECK(rb_ep_close_step(&P[0].b, 1, 0, T) == 0 && strstr(logbuf, "END OF CO-OP: the stock end flow runs on this PC"));
            tick(1000);
            CHECK(rb_ep_close_step(&P[0].b, 0, 0, T) == 1 && strstr(logbuf, "END OF CO-OP: the session closes now (the stock end flow (result, save) is over"));
            /* the solo part: 16-1 -> 16-2 is a normal section change (the second human spectates), the end of the extra episode closes co-op */
            sim_start();
            S.phase = 3;
            rb_triple nx = {0x91100200u, 256, 1};
            CHECK(rb_section_change(&P[0].b, 0x91100100u, 0x91100200u, &nx) == RB_SC_SENT && !P[0].b.ep_local && P[0].b.sec_sent == 1);
            tick(30);
            CHECK(P[1].b.sec_pending && P[1].b.rs_area == 0x91100200u && !P[1].b.ep_peer);
            CHECK(rb_section_change(&P[0].b, 0x91100200u, 0x91FF0000u, NULL) == RB_SC_EPEND && P[0].b.ep_kind == RB_EK_CAMPAIGN);
            tick(30);
            CHECK(P[1].b.ep_peer && P[1].b.ep_kind == RB_EK_CAMPAIGN);
            /* a non-section next */
            sim_start();
            S.phase = 3;
            CHECK(rb_section_change(&P[0].b, 0x91080300u, 0x91080400u, NULL) == RB_SC_EPEND && P[0].b.ep_kind == RB_EK_NOTSEC);
            /* a change from an unreadable section is taken as a paired one; from a non-section (a lab warp into F1) nothing happens */
            sim_start();
            S.phase = 3;
            rb_triple n2 = {0x91020200u, 256, 1};
            CHECK(rb_section_change(&P[0].b, 0, 0x91020200u, &n2) == RB_SC_SENT);
            CHECK(rb_section_change(&P[0].b, 0x910F0100u, 0x91020200u, &n2) == RB_SC_NONE);
            /* a reset forgets the kind */
            rb_reset(&P[0].b);
            CHECK(P[0].b.ep_kind == 0);
        }

        /* (g) the status line names the refusals by id */
        {
            char st[4000];
            sim_start();
            rb_status(&P[0].b, st, sizeof st);
            CHECK(strstr(st, "all episodes: starts refused by id (not a section 0), end kind 0"));
        }
    }

    /* ============================================ 8b. rev1 live 8: the cross-level guard (ef4 07:57:18, the client crash exe+9d344 at 07:59:49) */
    {   /* ef4: the client went on into 1-3 while the host's level (1-2) stayed. The stale fallback made the host report the client's new epoch loaded: both PCs PLAYED
         * one epoch in two levels (the 1-3 Jill puppet was teleported to the host's 1-2 spot, the host's 1-2 digest cleared 1-3's start flags) and the client crashed
         * tearing that level down at the host's next restart. Model: the client's LOAD_BEGIN names its next section; the host's level stays. */
        const rb_triple s13 = {0x91010300u, 256, 2}, s12 = {0x91010200u, 512, 0};
        int open_new = -1, open_old = -1;
        for (int pass = 0; pass < 2; pass++) {   /* pass 0: the model before the fix (xl_guard 0), pass 1: the product default */
            sim_start();
            for (int i = 0; i < 2; i++) P[i].b.cfg.xl_guard = pass, P[i].b.lvl_area = 0x91010200u;   /* both in 1-2 (as after the host's story start / the follow) */
            for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
            CHECK(S.phase == 3 && S.epoch == 1);
            logbuf[0] = 0;
            CHECK(rb_section_change(&P[1].b, 0x91010200u, 0x91010300u, NULL) == RB_SC_NOTED && P[1].b.lvl_area == 0x91010300u);   /* the client's own change */
            int opened = -1;
            for (int i = 0; i < 30000; i++) {
                tick(1);
                rb_load_step(&P[0].b, 1);                               /* the host's 1-2 level stays */
                rb_load_step(&P[1].b, i < 2000 ? 0 : 1);                /* the client's level goes away for 2 s, then 1-3 is there */
                if (opened < 0 && S.phase == 3 && S.epoch == 2) opened = i;
            }
            CHECK(S.load_area[1] == 0x91010300u && S.load_area_epoch[1] == 2);   /* the client's LOAD_BEGIN named its next section */
            if (pass == 0) open_old = opened;
            else open_new = opened;
        }
        CHECK(open_old >= 0);   /* the model before the fix: the host joined the client's 1-3 epoch with its 1-2 level (the cross-level epoch of ef4) */
        CHECK(open_new < 0 && S.phase == 2 && P[0].b.loads_xl == 1 && P[0].b.loads_stale == 0);   /* the guard: no cross-level epoch, refused once */
        CHECK(strstr(logbuf, "the other PC loads area 91010300, this PC's level is 91010200: NOT reported loaded") != NULL && strstr(logbuf, "reporting loaded anyway") == NULL);
        printf("  8b cross-level: before the fix the epoch opened after %d ms with two levels; with the guard it waits (refused %u)\n", open_old, P[0].b.loads_xl);
        /* the host reaches its own end later: its section change (1-3), its level goes away, it joins the epoch, both in 1-3 */
        CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT && P[0].b.lvl_area == 0x91010300u);
        for (int i = 0; i < 6000; i++) { tick(1); rb_load_step(&P[0].b, i < 1500 ? 0 : 1); rb_load_step(&P[1].b, 1); }
        CHECK(S.phase == 3 && S.epoch == 2 && P[0].b.loads_xl == 1);   /* (already LOADING: the host joins the epoch without a LOAD_BEGIN of its own) */
        /* the same section (a movie that took one PC's level away for 3 s): the stale fallback still lets the other PC report (no cross-level) */
        logbuf[0] = 0;
        unsigned st0 = P[1].b.loads_stale;
        for (int i = 0; i < 12000; i++) { tick(1); rb_load_step(&P[0].b, (i > 500 && i < 3500) ? 0 : 1); rb_load_step(&P[1].b, 1); }
        CHECK(S.phase == 3 && S.epoch == 3 && P[1].b.loads_stale == st0 + 1 && P[1].b.loads_xl == 0 && strstr(logbuf, "reporting loaded anyway") != NULL);
        /* an older peer (area 0 in its LOAD_BEGIN): the fallback as before */
        sim_start();
        P[0].b.lvl_area = 0x91010200u, P[1].b.lvl_area = 0;
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        for (int i = 0; i < 12000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, (i > 500 && i < 2500) ? 0 : 1); }
        CHECK(S.load_area[1] == 0 && S.phase == 3 && S.epoch == 2 && P[0].b.loads_stale == 1 && P[0].b.loads_xl == 0);
        /* E4 follow path: the host goes on into 1-3, the client follows within the stale window: no refusal, both in 1-3 (the direct call: the stock
         * section end is block 8d) */
        sim_start();
        for (int i = 0; i < 2; i++) P[i].b.lvl_area = 0x91010200u, P[i].b.cfg.sec_stock = 0;
        for (int i = 0; i < 2000; i++) { tick(1); rb_load_step(&P[0].b, 1); rb_load_step(&P[1].b, 1); }
        CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT);
        rb_triple fol, fol_eng;
        int followed = 0;
        for (int i = 0; i < 20000; i++) {
            tick(1);
            rb_load_step(&P[0].b, i < 1000 ? 0 : 1);
            if (!followed && rb_restart_follow_at(&P[1].b, &fol, 0) == 1) {   /* the glue calls FUN_00720870, the restart hook takes the triple */
                followed = i, fol_eng = s12;
                CHECK(rb_restart_client(&P[1].b, &fol_eng) == 1 && fol_eng.area == 0x91010300u && P[1].b.lvl_area == 0x91010300u);
            }
            rb_load_step(&P[1].b, followed && i < followed + 1500 ? 0 : 1);
        }
        if (!(followed >= 3900 && followed < 6000 && S.phase == 3 && S.epoch == 2)) printf("  (follow at %d, phase %d epoch %u, xl %u/%u, area %08X)\n", followed, S.phase, S.epoch, P[0].b.loads_xl, P[1].b.loads_xl, (unsigned)S.load_area[1]);
        CHECK(followed >= 3900 && followed < 6000 && S.phase == 3 && S.epoch == 2 && P[0].b.loads_xl == 0 && P[1].b.loads_xl == 0);   /* (the client joins the LOADING epoch without a LOAD_BEGIN of its own) */
        /* the host's restart names its triple's section, the client's restart without a host triple its own */
        sim_start();
        S.phase = 3;
        rb_triple h = {0x91010100u, 256, 4};
        CHECK(rb_restart_host(&P[0].b, &h) == 1 && P[0].b.lvl_area == 0x91010100u);
        rb_triple c = {0x91010300u, 257, 1};
        tick(1000);
        P[1].b.rs_have = 0;
        CHECK(rb_restart_client(&P[1].b, &c) == 0 && P[1].b.lvl_area == 0x91010300u);
        char st[2600];
        rb_status(&P[0].b, st, sizeof st);
        CHECK(strstr(st, "cross-level refused 0 (level 91010100)") != NULL);
        rb_reset(&P[0].b);
        CHECK(P[0].b.lvl_area == 0);
        /* the glue hands the other PC's LOAD_BEGIN area to the barrier, and the barrier sends its own level with its LOAD_BEGIN */
        {
            static char src[1 << 20];
            FILE *f = fopen("../native/rolebarrier1_game.c", "rb");
            size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
            if (f) fclose(f);
            src[n] = 0;
            CHECK(n > 10000 && strstr(src, "o->other_area = in.peer_load_area, o->other_area_epoch = (unsigned)in.peer_load_epoch;") != NULL);
            f = fopen("../native/rolebarrier1.c", "rb");
            n = f ? fread(src, 1, sizeof src - 1, f) : 0;
            if (f) fclose(f);
            src[n] = 0;
            CHECK(n > 10000 && strstr(src, "b->io.load_begin(b->io.user, b->lvl_area);") != NULL && strstr(src, "b->io.load_begin(b->io.user, 0)") == NULL);
        }
    }

    /* ============================================ 8c. rev1 live 8 (l8a 10:13): the SECTION follow after the session start's RESTART follow */
    {   /* l8a: the client followed the host's story start (RESTART #1, `client follows the host's restart`), later the host alone ended 1-2 and sent SECTION #1;
         * the client logged `SECTION #1 from the host ... this PC follows unless it changes there by itself` and then never followed (`followed 0`): the two
         * messages are numbered apart on the host (rs_tx_seq / sec_tx_seq) but the client kept both in rs_seq, and the follow's once-per-triple rule
         * (rs_follow_seq == rs_seq) took SECTION #1 for the RESTART #1 it had followed already. The model before the fix (this test built with the
         * rolebarrier1.c of 3035cba) fails the four follow checks below ("fol == 1 ..."), as l8a did live. */
        const rb_triple s11 = {0x91010100u, 256, 1}, s12 = {0x91010200u, 512, 0}, s13 = {0x91010300u, 256, 2};
        rb_triple out, eng;
        sim_start();
        S.phase = 3;
        P[1].b.cfg.sec_stock = 0;   /* the numbers only here; the stock section end is block 8d */
        logbuf[0] = 0;
        CHECK(rb_restart_host(&P[0].b, &s11) == 1 && P[0].b.rs_tx_seq == 1);   /* the host's story start = RESTART #1 */
        tick(2000);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 1 && out.area == 0x91010100u && P[1].b.rs_follow_seq == 1);   /* the client follows it (the session start) */
        eng = s11;
        CHECK(rb_restart_client(&P[1].b, &eng) == 1);
        tick(60000);
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT && P[0].b.sec_tx_seq == 1);   /* the host alone at the end of 1-1: SECTION #1 */
        tick(50);
        CHECK(P[1].b.sec_pending == 1 && P[1].b.rs_have == 1 && P[1].b.sec_rx_seq == 1);
        int old_rule_follows = !(P[1].b.rs_follow_seq == 1 /* = the old rs_seq of SECTION #1 */);   /* the model before the fix: the once-per-triple test of rs_seq */
        CHECK(old_rule_follows == 0);   /* before the fix: SECTION #1 counted as followed already (l8a: `followed 0`) */
        int fol = 0;
        for (int i = 0; i < 6000 && !fol; i++) { tick(1); fol = rb_restart_follow_at(&P[1].b, &out, 0); }
        CHECK(fol == 1 && out.area == 0x91010200u && out.a == 512 && P[1].b.sec_followed == 1);   /* now: the client follows the host into 1-2 after 4 s */
        CHECK(strstr(logbuf, "it follows the host from 91010100 into 91010200") != NULL);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);   /* once */
        eng = s11;
        CHECK(rb_restart_client(&P[1].b, &eng) == 1 && eng.area == 0x91010200u && !P[1].b.sec_pending);   /* the follow's call takes the section's triple */
        /* a later RESTART #2 (a Continue) is followed as before; then SECTION #2 again */
        tick(10000);
        CHECK(rb_restart_host(&P[0].b, &s12) == 1 && P[0].b.rs_tx_seq == 2);
        tick(2000);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 1 && out.area == 0x91010200u);
        eng = s12;
        CHECK(rb_restart_client(&P[1].b, &eng) == 1);
        tick(30000);
        CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT && P[0].b.sec_tx_seq == 2);
        fol = 0;
        for (int i = 0; i < 6000 && !fol; i++) { tick(1); fol = rb_restart_follow_at(&P[1].b, &out, 0); }
        CHECK(fol == 1 && out.area == 0x91010300u && P[1].b.sec_followed == 2);
        rb_reset(&P[1].b);
        CHECK(P[1].b.sec_follow_valid == 0 && P[1].b.sec_rx_seq == 0);
    }

    /* ============================================ 8d. rev1 live 10: the follow into the next section by this PC's own stock section end */
    {   /* l8c (FUN_00720870 from the running 1-2 level), l9b and l9d (FUN_00720870 from the title after FUN_008B1660) all loaded 1-3 WITHOUT its scene scripts;
         * l8d / l9a / l9c (FUN_00720870 at the title of a process that had played no level) loaded them. The engine model below is the static reading:
         * FUN_00720870 sets the restart bits (game flags [0x113AF8C] 1, 3) and keeps the checkpoint slot (sCheckPoint, count +0x18); the new area's opening
         * FUN_0041C8D0 returns at once ("restore from the checkpoint") when restart bit 1 is set and the slot is not empty, so a slot of ANOTHER section leaves
         * the new section without its scenes; its cEpisodeProcessState (FUN_0042DE90) resets the game flags only without the restart bit. The stock section
         * change (the final script's two section-end game flags -> cEpisodeMainState -> cEpisodeEndState_vf7) clears the slot (FUN_006D0F20) and needs no
         * restart bit. The model reproduces all six live loads; the model before the fix (sec_stock 0) answers RB_FOL_CALL in the level -> no scenes. */
        typedef struct { uint32_t area; int slot_count, restart_bit, gf_end, scenes, ended_at_once, calls720870, stock_ends; } eng_t;
        #define ENG_LOAD(e, a) do { (e)->area = (a); (e)->scenes = !((e)->restart_bit && (e)->slot_count > 0); \
                                    if (!(e)->restart_bit) (e)->gf_end = 0; (e)->ended_at_once = (e)->gf_end; (e)->restart_bit = 0; (e)->slot_count = 1; } while (0)
        #define ENG_RESTART(e, a) do { (e)->calls720870++; (e)->restart_bit = 1; ENG_LOAD(e, a); } while (0)
        #define ENG_STOCK_END(e, next) do { (e)->stock_ends++; (e)->slot_count = 0; (e)->gf_end = 0; ENG_LOAD(e, next); } while (0)
        eng_t m;
        /* the six live loads */
        memset(&m, 0, sizeof m); ENG_RESTART(&m, 0x91010300u); CHECK(m.scenes == 1);                                  /* l8d / l9a / l9c: a fresh title */
        memset(&m, 0, sizeof m); ENG_RESTART(&m, 0x91010200u); ENG_RESTART(&m, 0x91010300u); CHECK(m.scenes == 0);   /* l8c: from the running 1-2 level */
        memset(&m, 0, sizeof m); ENG_RESTART(&m, 0x91010200u); m.area = 0; ENG_RESTART(&m, 0x91010300u); CHECK(m.scenes == 0);   /* l9b / l9d: the title after FUN_008B1660 keeps the slot */
        memset(&m, 0, sizeof m); ENG_RESTART(&m, 0x91010200u); ENG_STOCK_END(&m, 0x91010300u); CHECK(m.scenes == 1);  /* the hosts of l8c / l9d: their own section end */
        const rb_triple s12 = {0x91010200u, 512, 0}, s13 = {0x91010300u, 256, 2};
        rb_triple out, eng;
        int scenes[2] = {-1, -1}, calls[2] = {0, 0};
        for (int pass = 0; pass < 2; pass++) {   /* 0 = the model before the fix, 1 = sec_stock */
            sim_start();
            S.phase = 3;
            for (int i = 0; i < 2; i++) P[i].b.cfg.sec_stock = pass;
            memset(&m, 0, sizeof m);
            ENG_RESTART(&m, 0x91010200u);    /* the session start's follow: the client plays 1-2 */
            P[1].b.lvl_area = 0x91010200u;
            logbuf[0] = 0;
            CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT);   /* the host alone ends 1-2 */
            for (int i = 0; i < 40000 && m.area != 0x91010300u; i++) {
                tick(1);
                int a = rb_restart_follow_at(&P[1].b, &out, 0);   /* in its level */
                if (a == RB_FOL_CALL) {
                    eng = s12;
                    CHECK(rb_restart_client(&P[1].b, &eng) == 1);
                    ENG_RESTART(&m, eng.area);
                }
                if (a == RB_FOL_SECEND) m.gf_end = 1;                    /* the glue: the two section-end game flags */
                if (m.gf_end && m.area == 0x91010200u && i % 16 == 15) {   /* cEpisodeMainState sees them a frame later: the stock change */
                    CHECK(rb_section_change(&P[1].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_NOTED);
                    ENG_STOCK_END(&m, 0x91010300u);
                }
            }
            scenes[pass] = m.scenes, calls[pass] = m.calls720870;
            if (pass == 1) {
                CHECK(m.area == 0x91010300u && m.stock_ends == 1 && m.calls720870 == 1 && !m.ended_at_once);   /* only the session start's call */
                CHECK(P[1].b.sec_stock_count == 1 && P[1].b.sec_followed == 0 && P[1].b.sec_consumed == 1 && !P[1].b.sec_pending && !P[1].b.rs_have && P[1].b.lvl_area == 0x91010300u);
                CHECK(strstr(logbuf, "comes by this PC's OWN STOCK section end") != NULL && strstr(logbuf, "= the host's waiting next section: no follow needed") != NULL);
                int more = 0;
                for (int i = 0; i < 40000; i++) { tick(1); more |= rb_restart_follow_at(&P[1].b, &out, 0); }
                CHECK(more == 0);   /* nothing twice */
            }
        }
        CHECK(scenes[0] == 0 && calls[0] == 2);   /* before the fix: FUN_00720870 from the 1-2 level, 1-3 without scenes (l8c) */
        CHECK(scenes[1] == 1 && calls[1] == 1);   /* now: the stock section change, 1-3 with its scenes */
        printf("  section follow: before the fix the 1-3 scenes %d (FUN_00720870 calls %d), with the stock section end %d (calls %d)\n", scenes[0], calls[0], scenes[1], calls[1]);
        /* the stock end does not come (the flags were set but no section change follows): the restart call after RB_SECEND_GIVEUP_MS, the flags cleared first;
         * without the clear the restart bit would keep them and the new level would end at once */
        for (int clear = 0; clear < 2; clear++) {
            sim_start();
            S.phase = 3;
            memset(&m, 0, sizeof m);
            ENG_RESTART(&m, 0x91010200u);
            P[1].b.lvl_area = 0x91010200u;
            logbuf[0] = 0;
            CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT);
            int a = 0;
            uint32_t t_sec = 0, t_call = 0;
            for (int i = 0; i < 60000 && !t_call; i++) {
                tick(1);
                a = rb_restart_follow_at(&P[1].b, &out, 0);
                if (a == RB_FOL_SECEND) m.gf_end = 1, t_sec = T;
                if (a == RB_FOL_CALL) {
                    t_call = T;
                    if (clear) m.gf_end = 0;   /* the glue: section_end_flags(0) before the call */
                    eng = s12;
                    CHECK(rb_restart_client(&P[1].b, &eng) == 1);
                    ENG_RESTART(&m, eng.area);
                }
            }
            CHECK(t_sec && t_call && t_call - t_sec >= RB_SECEND_GIVEUP_MS && t_call - t_sec < RB_SECEND_GIVEUP_MS + 50 && out.area == 0x91010300u);
            CHECK(P[1].b.sec_stock_fallback == 1 && strstr(logbuf, "the stock section end did not come") != NULL);
            CHECK(m.ended_at_once == !clear);   /* without the clear the new 1-3 would end at once */
        }
        /* not in the section the host left (another section, an unknown level, the title): the direct call as before */
        for (int k = 0; k < 3; k++) {
            sim_start();
            S.phase = 3;
            P[1].b.lvl_area = k == 0 ? 0x91010100u : 0;
            CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT);
            int a2 = 0;
            for (int i = 0; i < 6000 && !a2; i++) { tick(1); a2 = rb_restart_follow_at(&P[1].b, &out, k == 2); }
            CHECK(a2 == RB_FOL_CALL && P[1].b.sec_stock_count == 0);
        }
        /* a new SECTION (the next one) asks the stock end again */
        sim_start();
        S.phase = 3;
        P[1].b.lvl_area = 0x91010100u;
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT);
        int a3 = 0;
        for (int i = 0; i < 6000 && a3 != RB_FOL_SECEND; i++) { tick(1); a3 = rb_restart_follow_at(&P[1].b, &out, 0); }
        CHECK(a3 == RB_FOL_SECEND && P[1].b.sec_stock_seq == P[1].b.sec_rx_seq);
        CHECK(rb_section_change(&P[1].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_NOTED && !P[1].b.sec_pending);
        tick(100000);
        CHECK(rb_section_change(&P[0].b, 0x91010200u, 0x91010300u, &s13) == RB_SC_SENT);
        a3 = 0;
        for (int i = 0; i < 6000 && a3 != RB_FOL_SECEND; i++) { tick(1); a3 = rb_restart_follow_at(&P[1].b, &out, 0); }
        CHECK(a3 == RB_FOL_SECEND && P[1].b.sec_stock_count == 2);
        rb_reset(&P[1].b);
        CHECK(P[1].b.sec_stock_asked == 0);
        #undef ENG_LOAD
        #undef ENG_RESTART
        #undef ENG_STOCK_END
    }

    /* ============================================ 8e. rev1 live 10: the glue sets the section-end game flags the way FsmSetGameFlag does */
    {   /* the final script of 1-2 (12_00_event_s200_obr n3) and of 1-1 / 1-3: FsmSetGameFlag mType=2021257430 mFlagNo=2 mIsSet=1 and mType=505085882 mFlagNo=2;
         * FsmSetGameFlag 0x747880: obj = FUN_0078EB80([0x1169110], mType); obj->vf+0x14(mFlagNo) (set) / vf+0x18 (clear) */
        CHECK(RB_GF_TYPE_EPISODE == 2021257430u && RB_GF_TYPE_CUT == 505085882u && RB_GF_SECTION_END == 2u && RB_FOL_SECEND != RB_FOL_CALL && RB_FOL_SECEND != 1);
        static char src[1 << 20];
        FILE *f = fopen("../native/rolebarrier1_game.c", "rb");
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(n > 10000 && strstr(src, "int secend = folr == RB_FOL_SECEND;") != NULL && strstr(src, "if (section_end_flags(1)) g_gf_set = 1, g_gf_sets++;") != NULL);
        const char *fc = strstr(src, "if (follow && g_gf_set) {");
        const char *call = strstr(src, "if (follow) {   /* outside the lock");
        CHECK(fc && call && fc < call && strstr(fc, "section_end_flags(0);") && strstr(fc, "section_end_flags(0);") < call);   /* the flags are cleared before the fallback call */
        CHECK(strstr(src, "if (g_gf_set && !follow && !B.sec_pending) g_gf_set = 0;") != NULL);   /* (l10a: "(set now)" stayed after the stock change) */
        CHECK(strstr(src, "((unsigned(TC *)(unsigned, unsigned))(uintptr_t)GF_LOOKUP_VA)(holder, type);") != NULL &&
              strstr(src, "rd32(vt + (set ? 0x14u : 0x18u))") != NULL && strstr(src, "#define GF_HOLDER_PTR 0x1169110u") != NULL);
        CHECK(strstr(src, "rb_title_follow_pending") == NULL && strstr(src, "menu_quit_to_title") == NULL && strstr(src, "RB_FOL_TITLE") == NULL);   /* the title route is gone */
        f = fopen("../native/menu.c", "rb");
        n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(n > 10000 && strstr(src, "rolebarrier_title_follow_pending") == NULL && strstr(src, "if (insession && g_played && !level && (idle || title_area())) {") != NULL);
        /* the bytes the glue checks are those of the image (when it is there) */
        static const uint8_t lk[16] = {0x53, 0x57, 0x8b, 0xd9, 0x33, 0xc0, 0x33, 0xff, 0x39, 0x43, 0x28, 0x76, 0x2d, 0x55, 0x8b, 0x6c};
        static const uint8_t sg[12] = {0xa1, 0x10, 0x91, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14, 0x68, 0xf8, 0xb0};
        f = fopen("../native/rolebarrier1_game.c", "rb");
        n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(strstr(src, "gf_lookup_bytes[16] = {0x53, 0x57, 0x8b, 0xd9, 0x33, 0xc0, 0x33, 0xff, 0x39, 0x43, 0x28, 0x76, 0x2d, 0x55, 0x8b, 0x6c};") != NULL &&
              strstr(src, "gf_setflag_bytes[12] = {0xa1, 0x10, 0x91, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14, 0x68, 0xf8, 0xb0};") != NULL);
        const char *path = getenv("REV1_IMAGE");
        if (!path || !*path) path = "../../image/rerev-image.bin";
        FILE *im = fopen(path, "rb");
        if (im) {
            uint8_t b1[16], b2[12];
            int ok = fseek(im, 0x78EB80L - 0x400000L, SEEK_SET) == 0 && fread(b1, 1, 16, im) == 16 && fseek(im, 0x747880L - 0x400000L, SEEK_SET) == 0 && fread(b2, 1, 12, im) == 12;
            fclose(im);
            CHECK(ok && !memcmp(b1, lk, 16) && !memcmp(b2, sg, 12));
        } else printf("  (no image at %s: the game flag bytes are not checked against it)\n", path);
    }

    /* ============================================ rev1 live 15: the guest's own save read before its title follow (m14d / m14h / m14j: the empty bag) */
    {
        uint8_t m[RB_MSG_MAX];
        rb_cp cp = {1, 2, 3}, nocp = {0, 0, 0};
        CHECK(RB_RESTART_K_LEN <= RB_MSG_MAX && RB_RESTART_K_LEN <= CP_EVENT_MAX);
        CHECK(rb_msg_restart_k(m, sizeof m, 5, 0x91010300u, 256, 2, 1, &cp, RB_KIND_SAVE) == RB_RESTART_K_LEN && m[1] == RBM_RESTART && m[16] == 1 && m[21] == (RB_KIND_SAVE | 4));
        CHECK(rb_msg_restart_k(m, 21, 5, 0x91010300u, 256, 2, 1, &cp, RB_KIND_SAVE) == 0);
        /* the start kind of FUN_00720540's callers */
        CHECK(rb_start_kind(0x6D11D5u) == RB_KIND_SAVE && rb_start_kind(0x6D1188u) == RB_KIND_SAVE && rb_start_kind(0x6D0DEEu) == RB_KIND_SAVE);
        CHECK(rb_start_kind(0x83E89Bu) == RB_KIND_NEW && rb_start_kind(0x83BEFEu) == RB_KIND_NEW && rb_start_kind(0x83C0D1u) == RB_KIND_NEW);
        CHECK(rb_start_kind(0x500000u) == RB_KIND_UNKNOWN && rb_start_kind(0u) == RB_KIND_UNKNOWN);
        /* the guest reads the kind; the older lengths are "unknown" */
        sim_start();
        CHECK(rb_on_event(&P[1].b, m, RB_RESTART_K_LEN) == 1 && P[1].b.rs_kind == RB_KIND_SAVE && P[1].b.rs_cp.have && P[1].b.rs_cp.id == 2 && P[1].b.rs_cp.mask == 3 &&
              P[1].b.rs_diff == 1 && P[1].b.rs_area == 0x91010300u && P[1].b.rs_a == 256 && P[1].b.rs_b == 2);
        CHECK(rb_msg_restart_k(m, sizeof m, 6, 0x91010100u, 256, 1, -1, &nocp, RB_KIND_NEW) == RB_RESTART_K_LEN && m[21] == RB_KIND_NEW && m[16] == 0xFF);
        CHECK(rb_on_event(&P[1].b, m, RB_RESTART_K_LEN) == 1 && P[1].b.rs_kind == RB_KIND_NEW && !P[1].b.rs_cp.have && P[1].b.rs_diff == -1);
        CHECK(rb_msg_restart_cp(m, sizeof m, 7, 0x91010300u, 256, 2, 1, &cp) == RB_RESTART_CP_LEN && rb_on_event(&P[1].b, m, RB_RESTART_CP_LEN) == 1 && P[1].b.rs_kind == RB_KIND_UNKNOWN &&
              P[1].b.rs_cp.have);
        /* what the host sends: its restart hook = SAVE, the title menu's New Game = NEW, the sCheckPoint start (a 1-2 save's title Continue) = SAVE */
        sim_start();
        rb_triple t13 = {0x91010300u, 256, 2}, t11 = {0x91010100u, 256, 1}, t12 = {0x91010200u, 40, 3};
        CHECK(rb_restart_host(&P[0].b, &t13) == 1 && P[0].b.tx_kind == RB_KIND_SAVE);
        tick(30);
        CHECK(P[1].b.rs_kind == RB_KIND_SAVE && P[1].b.rs_area == 0x91010300u);
        tick(3500);
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1 && rb_start_triple(&P[0].b, &t11) == RB_ST_SENT && P[0].b.tx_kind == RB_KIND_NEW && P[0].b.tx_kind_once == 0);
        tick(30);
        CHECK(P[1].b.rs_kind == RB_KIND_NEW && P[1].b.rs_area == 0x91010100u);
        tick(3500);
        CHECK(rb_start_seen(&P[0].b, 0x6D1188u) == 1 && rb_start_triple(&P[0].b, &t12) == RB_ST_SENT && P[0].b.tx_kind == RB_KIND_SAVE);
        tick(30);
        CHECK(P[1].b.rs_kind == RB_KIND_SAVE && P[1].b.rs_area == 0x91010200u);
        CHECK(rb_restart_host(&P[0].b, &t13) == 1 && P[0].b.tx_kind == RB_KIND_SAVE);   /* the next restart hook send is SAVE again */
        /* the feature off = the old 21-byte form (the model before the fix) */
        P[0].b.cfg.title_load = 0;
        P[0].b.tx_cp.have = 1;
        unsigned c0 = P[0].b.cp_sent;
        tick(3500);
        CHECK(rb_restart_host(&P[0].b, &t13) == 1 && P[0].b.cp_sent == c0 + 1);
        tick(30);
        CHECK(P[1].b.rs_kind == RB_KIND_UNKNOWN && P[1].b.rs_cp.have);
        P[0].b.cfg.title_load = 1;
        c0 = P[0].b.cp_sent;
        tick(3500);
        CHECK(rb_restart_host(&P[0].b, &t13) == 1 && P[0].b.cp_sent == c0 + 1);   /* the 22-byte form still counts its record */
        tick(30);
        CHECK(P[1].b.rs_kind == RB_KIND_SAVE && P[1].b.rs_cp.have);
        P[0].b.tx_cp.have = 0;
        /* a SECTION triple is no save start */
        uint8_t sm_[RB_MSG_MAX];
        CHECK(rb_msg_section(sm_, sizeof sm_, 1, &t12, 0x91010100u, RB_SEC_PEER) == RB_SECTION_MSG_LEN && rb_on_event(&P[1].b, sm_, RB_SECTION_MSG_LEN) == 1 && P[1].b.rs_kind == RB_KIND_UNKNOWN);

        /* the scenario: the host's title Continue of a 1-3 save, the guest at the title */
        rb_triple tr;
        int asked = 0;
        CHECK(tl_run(1, 0, 1200, 0, &tr, &asked) == 3);   /* the fix: the guest's human gets its SAVED bag */
        CHECK(asked == 1 && tr.area == 0x91010300u && tr.a == 256 && tr.b == 2 && P[1].b.tl_done == 1 && P[1].b.tl_state == RB_TLS_DONE);
        CHECK(strstr(logbuf, "title follow: the host's start is a SAVE (its title Continue): this PC reads its own save first") != NULL);
        CHECK(strstr(logbuf, "own save read is DONE after") != NULL);
        /* the model before the fix (title_load 0 = the direct FUN_00720870 at once): the EMPTY bag of m14d / m14h / m14j */
        CHECK(tl_run(0, 0, 1200, 0, &tr, &asked) == 0 && asked == 0 && tr.area == 0x91010300u);
        /* New Game: no read, the New Game set, the call at once */
        CHECK(tl_run(1, 1, 1200, 0, &tr, &asked) == 7 && asked == 0 && P[1].b.tl_skipped == 1 && tr.area == 0x91010100u && tr.a == 256 && tr.b == 1);
        /* the read cannot be asked (no title menu manager / no slot 0 save): the direct call at once (the old behaviour, logged) */
        CHECK(tl_run(1, 0, 1200, 1, &tr, &asked) == 0 && asked == 1 && P[1].b.tl_refused == 1 && tr.area == 0x91010300u);
        /* the read never ends: the direct call after title_load_ms (the guest is never stuck at the title) */
        P[1].b.cfg.title_load_ms = 15000;
        CHECK(tl_run(1, 0, -1, 0, &tr, &asked) == 0 && asked == 1 && P[1].b.tl_timeout == 1 && tr.area == 0x91010300u);
        /* a retry while the read runs is held; leaving the title forgets the state; the next title stay reads again */
        tl_run(1, 0, 1200, 0, &tr, &asked);
        P[1].b.tl_state = RB_TLS_WAIT;
        CHECK(rb_title_load_gate(&P[1].b, &tr, 1) == RB_TL_WAIT);
        rb_triple o2;
        CHECK(rb_title_load_frame(&P[1].b, 0, 0, 0, &o2) == 0 && P[1].b.tl_state == RB_TLS_IDLE);
        CHECK(rb_title_load_gate(&P[1].b, &tr, 0) == RB_TL_CALL);   /* in a level: never a read */
        P[1].b.rs_kind = RB_KIND_SAVE;
        CHECK(rb_title_load_gate(&P[1].b, &tr, 1) == RB_TL_LOAD && P[1].b.tl_asked == 2);
        P[1].b.host = 1;
        P[1].b.tl_state = RB_TLS_IDLE;
        CHECK(rb_title_load_gate(&P[1].b, &tr, 1) == RB_TL_CALL);   /* the host never reads */
        P[1].b.host = 0;
        rb_reset(&P[1].b);
        CHECK(P[1].b.tl_state == RB_TLS_IDLE && P[1].b.rs_kind == RB_KIND_UNKNOWN);
        /* the glue: the read is asked outside the lock before the follow's call, its bytes are the image's, the retry waits for the read */
        static char src[1 << 20];
        FILE *f = fopen("../native/rolebarrier1_game.c", "rb");
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        const char *ask = strstr(src, "const char *why = title_read_ask();"), *call = strstr(src, "if (follow) {   /* outside the lock");
        CHECK(ask && call && ask < call);
        CHECK(strstr(src, "int g = rb_title_load_gate(&B, &fol, at_title);") != NULL && strstr(src, "B.tl_state != RB_TLS_ASK &&") != NULL);
        CHECK(strstr(src, "((void(TC *)(unsigned, unsigned))(uintptr_t)TL_SLOT_READ_VA)(runner, 0);") != NULL && strstr(src, "if (hd == 1) {") != NULL);
        static const struct { uint32_t va; uint8_t b[8]; } tlb[] = {
            {0x8B5D20u, {0x51, 0x8B, 0x44, 0x24, 0x08, 0x50, 0x8D, 0x54}}, {0x81F940u, {0x83, 0x79, 0x30, 0x00, 0x75, 0x0C, 0x83, 0x79}},
            {0x8B3430u, {0xA1, 0x28, 0xA3, 0x16, 0x01, 0x81, 0xEC, 0x80}}, {0x8B9260u, {0x8B, 0x44, 0x24, 0x04, 0xC7, 0x81, 0xEC, 0x00}}};
        CHECK(strstr(src, "TL_B_ENTRY[] = {0x51, 0x8B, 0x44, 0x24, 0x08, 0x50, 0x8D, 0x54};") && strstr(src, "TL_B_OK[] = {0x83, 0x79, 0x30, 0x00, 0x75, 0x0C, 0x83, 0x79};") &&
              strstr(src, "TL_B_SLOT[] = {0xA1, 0x28, 0xA3, 0x16, 0x01, 0x81, 0xEC, 0x80};") && strstr(src, "TL_B_CACHE[] = {0x8B, 0x44, 0x24, 0x04, 0xC7, 0x81, 0xEC, 0x00};"));
        const char *path = getenv("REV1_IMAGE");
        if (!path || !*path) path = "../../image/rerev-image.bin";
        FILE *im = fopen(path, "rb");
        if (im) {
            for (int i = 0; i < 4; i++) {
                uint8_t b[8];
                CHECK(fseek(im, (long)tlb[i].va - 0x400000L, SEEK_SET) == 0 && fread(b, 1, 8, im) == 8 && !memcmp(b, tlb[i].b, 8));
            }
            /* the title menu's Continue: [0x116A328] +0x5C0 == 1 -> FUN_008B5D20(0) -> FUN_0081F940 -> FUN_008B3430(ecx = [esi + 0x68], 0) (0x83DCAE..0x83DCFE) */
            uint8_t c[0x52];
            static const uint8_t cmp5c0[7] = {0x80, 0xB8, 0xC0, 0x05, 0x00, 0x00, 0x01}, mgr68[3] = {0x8B, 0x4E, 0x68};
            CHECK(fseek(im, 0x83DCAEL - 0x400000L, SEEK_SET) == 0 && fread(c, 1, sizeof c, im) == sizeof c && !memcmp(c, cmp5c0, 7) && !memcmp(c + 0x4B, mgr68, 3) && c[0x4E] == 0x6A &&
                  c[0x4F] == 0x00 && c[0x50] == 0xE8);
            fclose(im);
        } else printf("  (no image at %s: the title read bytes are not checked against it)\n", path);
    }

    printf("test_rolebarrier1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
