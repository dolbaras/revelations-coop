/*
 * Offline test of native/prompt1.h (E8, ep1-probe): a story prompt pressed by one human counts on both PCs.
 *   Model: two PCs, each with 16 PCS slots, an action command manager that turns the LOCAL pad's A into status 2 of the registered (shown) command and
 *   keeps a status otherwise (cActionCommand_vf5), and a prompt script like 11_10b_0 (the grating): it starts on a PC when a unit stands in its zone on
 *   that PC, shows command 0, waits for status 2, then ends a few frames later. The line between the PCs delivers each event once (the world1 ext lane is
 *   reliable) after a latency, in order or swapped.
 *   The model before the fix (no mirror) must fail: the copy of the PC whose human did not press never ends.
 *   sh tests/build.sh   ->  tests/build/test_prompt1.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/prompt1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define FRAME 16u
typedef struct {
    pr1_view slot[PR1_SLOTS];       /* the engine's slots as the glue reads them */
    int running[PR1_SLOTS];         /* model: script state 0 none, 1 waiting for the press, 2 pressed (ends after a few frames) */
    int end_in[PR1_SLOTS];
    int ended;                      /* how many prompt scripts ended on this PC */
    int pad_a;                      /* the local pad's A this frame */
    int frozen;                     /* the action command manager does not update (1-3 solo part: the own human switched off) */
    int mirror;
    int forced_read;                /* the getter hook's pending answer (prompt1_game.inc hook_pcsact): the FSM's next read of command 0 says 2 */
    pr1_t p;
    unsigned writes;
} pc_t;

#define QN 64
typedef struct { uint8_t b[8]; unsigned n; uint32_t due; int to; } msg_t;
static msg_t q[QN];
static unsigned nq;
static uint32_t g_lat = 40;
static int g_swap;

static void send(int to, const uint8_t *b, unsigned n, uint32_t now)
{
    if (nq >= QN) return;
    memcpy(q[nq].b, b, n), q[nq].n = n, q[nq].due = now + g_lat + (g_swap && (nq & 1) ? 30u : 0u), q[nq].to = to, nq++;
}

static const uint32_t H_GRATE = 0;   /* set in main: pr1_hash of the script */
static uint32_t h_grate, h_other;

static void start_script(pc_t *c, int s, uint32_t h)
{
    memset(&c->slot[s], 0, sizeof c->slot[s]);
    c->slot[s].hash = h, c->slot[s].act[0] = 1;   /* PcsStartACTriggerButton: command 0 registered (shown) */
    c->running[s] = 1, c->end_in[s] = 0;
}

/* one engine frame: the manager (cActionCommand_vf5 of a shown command: a press of the local pad -> 2, released -> 3: e1p3 live, a status written between frames
 * is gone before the FSM reads it), then the script (its read: status 2 -> pressed, ends 3 frames later) */
static int g_hook = 1;   /* the glue answers through the getter hook (0 = the status write alone: the model before the fix of e1p3) */
static void engine(pc_t *c)
{
    for (int s = 0; s < PR1_SLOTS; s++) {
        pr1_view *v = &c->slot[s];
        if (!v->hash) continue;
        if (!c->frozen && v->act[0]) v->st[0] = c->pad_a ? PR1_PRESSED : (v->st[0] == PR1_PRESSED ? 3 : v->st[0]);
        int read2 = v->st[0] == PR1_PRESSED || (c->forced_read && v->act[0]);
        c->forced_read = 0;
        if (c->running[s] == 1 && read2) c->running[s] = 2, c->end_in[s] = 3, v->act[0] = 0;   /* PcsStopAC */
        else if (c->running[s] == 2 && --c->end_in[s] <= 0) { memset(v, 0, sizeof *v); c->running[s] = 0; c->ended++; }
    }
}

/* the glue's pass (prompt_pass of prompt1_game.inc) */
static void glue(pc_t *c, int me, uint32_t now)
{
    if (!c->mirror) return;
    pr1_ev out[8];
    unsigned n = pr1_scan(&c->p, c->slot, now, out, 8);
    for (unsigned i = 0; i < n; i++) {
        uint8_t b[8];
        unsigned k = pr1_enc(b, sizeof b, out[i].hash, out[i].cmd);
        send(1 - me, b, k, now);
    }
    unsigned s, k;
    while (pr1_due(&c->p, c->slot, now, &s, &k)) {
        c->slot[s].st[k] = PR1_PRESSED, c->writes++;
        if (g_hook) c->forced_read = 1;
        pr1_forced(&c->p, c->slot[s].hash, k, now);
    }
}

static void deliver(pc_t *pc, uint32_t now)
{
    for (unsigned i = 0; i < nq;) {
        if ((int32_t)(now - q[i].due) >= 0) {
            uint32_t h;
            unsigned k;
            if (pc[q[i].to].mirror && pr1_dec(q[i].b, q[i].n, &h, &k)) pr1_remote(&pc[q[i].to].p, h, k, now);
            q[i] = q[--nq];
        } else
            i++;
    }
}

typedef struct {
    int mirror;
    uint32_t start_h, start_c;      /* when each PC's copy starts (ms); ~0 = never */
    uint32_t press_h, press_c;      /* when each PC's human presses A (ms); ~0 = never */
    uint32_t client_script;         /* the script the client runs (the same one or another) */
    int client_noact;               /* the client's command never gets shown */
    int client_frozen;
    unsigned epoch_reset_at;        /* 0 = none: pr1_reset on the client at that time */
    uint32_t run_ms;
} scen_t;

static void run(const scen_t *sc, pc_t pc[2])
{
    memset(pc, 0, 2 * sizeof(pc_t));
    nq = 0;
    pc[0].mirror = pc[1].mirror = sc->mirror;
    pc[1].frozen = sc->client_frozen;
    for (uint32_t t = 0; t < sc->run_ms; t += FRAME) {
        if (t == (sc->start_h / FRAME) * FRAME) start_script(&pc[0], 3, h_grate);
        if (t == (sc->start_c / FRAME) * FRAME) { start_script(&pc[1], 5, sc->client_script); if (sc->client_noact) pc[1].slot[5].act[0] = 0; }
        if (sc->epoch_reset_at && t == (sc->epoch_reset_at / FRAME) * FRAME) pr1_reset(&pc[1].p);
        pc[0].pad_a = sc->press_h != ~0u && t >= sc->press_h && t < sc->press_h + 100;
        pc[1].pad_a = sc->press_c != ~0u && t >= sc->press_c && t < sc->press_c + 100;
        deliver(pc, t);
        for (int i = 0; i < 2; i++) engine(&pc[i]);
        for (int i = 0; i < 2; i++) glue(&pc[i], i, t);
    }
}

int main(void)
{
    h_grate = pr1_hash("demo\\d0\\11_10b_0\\11_10b_0_main");
    h_other = pr1_hash("demo\\d0\\11_100_0\\11_100_0_main");
    (void)H_GRATE;
    pc_t pc[2];
    /* 1. the host's human presses at the grating; the client's copy (its Jill puppet in the zone) is shown too */
    scen_t a = {1, 1000, 1500, 3000, ~0u, 0, 0, 0, 0, 10000};
    a.client_script = h_grate;
    scen_t before = a;
    before.mirror = 0;
    run(&before, pc);
    int before_fails = 0;
    before_fails += !(pc[0].ended == 1);   /* fine without the fix */
    before_fails += !(pc[1].ended == 1);   /* the client's copy never ends without the fix */
    CHECK(pc[0].ended == 1 && pc[1].ended == 0);
    printf("  the model before the fix: host's copy ended %d, client's copy ended %d (fails %d of 2 checks)\n", pc[0].ended, pc[1].ended, before_fails);
    g_hook = 0;
    run(&a, pc);
    CHECK(pc[0].ended == 1 && pc[1].ended == 0);   /* the status write alone (e1p3 live): the client's copy stays at its prompt */
    printf("  the model before the getter hook: the client's copy ended %d (the manager undoes the written status)\n", pc[1].ended);
    g_hook = 1;
    run(&a, pc);
    CHECK(pc[0].ended == 1 && pc[1].ended == 1);
    CHECK(pc[0].p.announced == 1 && pc[1].p.applied == 1 && pc[1].writes == 1 && pc[0].writes == 0);
    CHECK(pc[1].p.announced == 0);   /* the forced press is not sent back (its rise is dropped as an echo, or the manager already turned it into 3) */
    CHECK(pc[0].p.rx == 0 && pc[1].p.rx == 1);
    /* 2. the client's human presses (roles the other way) */
    scen_t b = {1, 1000, 1200, ~0u, 2500, 0, 0, 0, 0, 10000};
    b.client_script = h_grate;
    run(&b, pc);
    CHECK(pc[0].ended == 1 && pc[1].ended == 1 && pc[0].p.applied == 1 && pc[1].p.applied == 0 && pc[1].p.announced == 1);
    /* 3. both press within the quiet window: nobody forces on top of an own press */
    scen_t c2 = {1, 1000, 1000, 3000, 3010, 0, 0, 0, 0, 10000};
    c2.client_script = h_grate;
    run(&c2, pc);
    CHECK(pc[0].ended == 1 && pc[1].ended == 1 && pc[0].writes == 0 && pc[1].writes == 0);
    CHECK(pc[0].p.own_dropped == 1 && pc[1].p.own_dropped == 1);
    /* 4. the host presses before the client's copy exists (its puppet reaches the zone 5 s later): kept, pressed when the copy is shown */
    scen_t d = {1, 1000, 8000, 3000, ~0u, 0, 0, 0, 0, 14000};
    d.client_script = h_grate;
    run(&d, pc);
    CHECK(pc[1].ended == 1 && pc[1].p.applied == 1 && pc[1].p.expired == 0);
    /* 5. ... but not after the keep time: the press is dropped, the late copy waits for its own human (as before the fix) */
    scen_t e = {1, 1000, 3000 + PR1_KEEP_MS + 500, 3000, ~0u, 0, 0, 0, 0, 20000};
    e.client_script = h_grate;
    run(&e, pc);
    CHECK(pc[1].ended == 0 && pc[1].p.applied == 0 && pc[1].p.expired == 1 && pc[1].p.npend == 0);
    /* 6. another script runs on the client: never pressed by the host's press */
    scen_t f = {1, 1000, 1500, 3000, ~0u, 0, 0, 0, 0, 10000};
    f.client_script = h_other;
    run(&f, pc);
    CHECK(pc[1].ended == 0 && pc[1].writes == 0);
    /* 7. the client's command is not shown (registered): not pressed */
    scen_t g = {1, 1000, 1500, 3000, ~0u, 0, 1, 0, 0, 10000};
    g.client_script = h_grate;
    run(&g, pc);
    CHECK(pc[1].writes == 0 && pc[1].ended == 0);
    /* 8. the client's action command manager is frozen (the 1-3 solo part: its own human switched off): the written status stays, its copy ends */
    scen_t h = {1, 1000, 1500, 3000, ~0u, 0, 0, 1, 0, 10000};
    h.client_script = h_grate;
    run(&h, pc);
    CHECK(pc[1].ended == 1 && pc[1].writes == 1);
    scen_t h0 = h;
    h0.mirror = 0;
    h0.press_c = 4000;   /* without the fix even the client's own press does nothing while frozen */
    run(&h0, pc);
    CHECK(pc[1].ended == 0);
    /* 9. a new level instance on the client drops what was pending */
    scen_t k = {1, 1000, 8000, 3000, ~0u, 0, 0, 0, 5000, 14000};
    k.client_script = h_grate;
    run(&k, pc);
    CHECK(pc[1].ended == 0 && pc[1].p.applied == 0 && pc[1].p.npend == 0);
    /* 10. late and swapped delivery */
    g_lat = 400, g_swap = 1;
    run(&a, pc);
    CHECK(pc[0].ended == 1 && pc[1].ended == 1 && pc[1].p.applied == 1);
    g_lat = 40, g_swap = 0;
    /* 11. wire and key */
    uint8_t buf[8];
    uint32_t hh;
    unsigned cc;
    CHECK(pr1_enc(buf, sizeof buf, 0xA1B2C3D4u, 5) == 5 && pr1_dec(buf, 5, &hh, &cc) && hh == 0xA1B2C3D4u && cc == 5);
    CHECK(pr1_enc(buf, 4, 1, 0) == 0 && pr1_enc(buf, sizeof buf, 1, PR1_CMDS) == 0 && !pr1_dec(buf, 4, &hh, &cc));
    buf[4] = PR1_CMDS;
    CHECK(!pr1_dec(buf, 5, &hh, &cc));
    CHECK(pr1_hash("demo\\d0\\11_10b_0\\11_10b_0_main") == pr1_hash("DEMO/D0/11_10B_0/11_10B_0_MAIN") && pr1_hash("a") != pr1_hash("b") && pr1_hash("") != 0);
    /* 12. a remote press with a bad command index / no hash is refused; a duplicate is kept once */
    pr1_t p;
    memset(&p, 0, sizeof p);
    CHECK(!pr1_remote(&p, 0, 0, 10) && !pr1_remote(&p, 5, PR1_CMDS, 10));
    CHECK(pr1_remote(&p, 5, 0, 10) && !pr1_remote(&p, 5, 0, 20) && p.npend == 1 && p.dup_dropped == 1);
    /* 13. the glue's wiring (source; CRLF-safe: one-line fragments only) */
    {
        static char src[1 << 20];
        const char *files[3] = {"../native/world1_game.c", "../native/story1_game.inc", "../native/prompt1_game.inc"};
        const char *need[3][4] = {
            {"#include \"prompt1_game.inc\"", "            prompt_pass(now);", "!strcmp(sub, \"pr\")", NULL},
            {"} else if (sub == CP_W1_PROMPT) {", "        prompt_rx(b, len, now);", NULL, NULL},
            {"static volatile int f_prompt = 1;", "#define PR_CMD_VTBL 0xFEBBA8u", "if (rd32(cmd) != PR_CMD_VTBL) continue;", "if (rd32(fsm + PR_UFSM + 4) != rec || !res) continue;"},
        };
        /* e1p3 live: the status write alone was undone by the action command manager before the FSM read it; the getter hook answers 2 once */
        {
            FILE *fp = fopen("../native/prompt1_game.inc", "rb");
            size_t n = fp ? fread(src, 1, sizeof src - 1, fp) : 0;
            if (fp) fclose(fp);
            src[n] = 0;
            CHECK(strstr(src, "if (ok && H.state[WH_PCSACT] == WH_ON) pr_force_k = k, pr_force_until = GetTickCount() + 3000u, pr_force_rec = recs[s];") != NULL);
            CHECK(strstr(src, "r = PR1_PRESSED;") != NULL && strstr(src, "pr_force_rec = 0;") != NULL);   /* answered once, then the request is gone */
            fp = fopen("../native/world1_game.c", "rb");
            n = fp ? fread(src, 1, sizeof src - 1, fp) : 0;
            if (fp) fclose(fp);
            src[n] = 0;
            CHECK(strstr(src, "{WH_PCSACT, (void *)hook_pcsact, (void **)&orig_pcsact},") != NULL);
        }
        for (int i = 0; i < 3; i++) {
            FILE *fp = fopen(files[i], "rb");
            size_t n = fp ? fread(src, 1, sizeof src - 1, fp) : 0;
            if (fp) fclose(fp);
            src[n] = 0;
            for (int j = 0; j < 4 && need[i][j]; j++) {
                int ok = strstr(src, need[i][j]) != NULL;
                CHECK(ok);
                if (!ok) printf("  missing in %s: %s\n", files[i], need[i][j]);
            }
        }
    }
    /* 14. the engine facts the glue reads (image, when it is there): the uFsm part of a uPcsFsm at + 0x30 (e1p2 live: + 0 read no slot) */
    {
        FILE *fi = fopen("../../image/rerev-image.bin", "rb");
        if (!fi) printf("  (no executable image: the uFsm offset is not checked)\n");
        else {
            uint8_t a[6], b2[3], c3[4];
            int ok = fseek(fi, 0x89171DL - 0x400000L, SEEK_SET) == 0 && fread(a, 1, 6, fi) == 6 && fseek(fi, 0x891868L - 0x400000L, SEEK_SET) == 0 && fread(b2, 1, 3, fi) == 3 &&
                     fseek(fi, 0xFEBBA8L - 0x400000L, SEEK_SET) == 0 && fread(c3, 1, 4, fi) == 4;
            CHECK(ok && a[0] == 0x8D && a[1] == 0x48 && a[2] == 0x30 && a[5] == 0xE8);   /* lea ecx,[eax+0x30]; push 0; call 0xA221A0 (the slot's resource) */
            CHECK(ok && b2[0] == 0x8D && b2[1] == 0x4E && b2[2] == 0x30);                /* lea ecx,[esi+0x30]; call 0xA25480 (record, resource) */
            CHECK(ok && c3[0] == 0x60 && c3[1] == 0xEF && c3[2] == 0x40 && c3[3] == 0x00); /* the cActionCommand vtable's first entry 0x40EF60 */
            fclose(fi);
        }
    }
    printf("test_prompt1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
