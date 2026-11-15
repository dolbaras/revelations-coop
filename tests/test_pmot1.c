/*
 * Offline test of native/pmot1.h (sync022 rows P5-P10 / P12): the partner's interaction motions on his puppet.
 * Two PCs on a line with a delay. The partner's PC runs a model of its own human (the interaction states with their motions: a pickup 0x47, the lever pull with
 * bank 0xE bound from the lever's list); this PC runs the receiver against a model of the puppet:
 *   - its lower state machine: 5 MoveState, 2 ExWaitState (the standing wait), 6 AboutFaceState (a turn), 0 EventWaitState, 37 a hit (the engine's own), deferred
 *     requests by index applied at the puppet's next tick;
 *   - its layer 0 (EventWaitState's enter sets the idle 0x0401, MoveState's 0x0001);
 *   - the bank resources: each object list has a reference count (1 by its object while loaded); the puppet's bank 0xE holds one reference while bound
 *     (FUN_00A2D7E0: release the old, add-ref the new; 0 = release); a stage unload drops the objects' own references; the puppet's despawn drops his;
 *     a reference count below 0 or the use of a freed list is a failure.
 * Checks: the puppet shows the partner's motion while the partner interacts and is back in MoveState after it; no call while a cutscene / load / downed holds him
 * (and his EventWaitState is released once that is over); a busy puppet is never asked; the refs are balanced across enter / exit / stage unload / despawn;
 * the model before the fix (0.2.1: no mirror) never shows the motion (MODEL BEFORE THE FIX FAILS).
 *   sh tests/build.sh -> tests/build/test_pmot1.exe
 */
#include <stdio.h>
#include <string.h>

#include "../native/pmot1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- resources */
#define NRES 4
static int ref[NRES], freed[NRES], uaf;
static void res_add(int r) { if (r <= 0) return; if (freed[r]) uaf++; ref[r]++; }
static void res_rel(int r)
{
    if (r <= 0) return;
    if (freed[r]) uaf++;
    if (--ref[r] < 0) uaf++;
    if (ref[r] == 0) freed[r] = 1;
}

/* ---- the puppet (this PC) */
typedef struct {
    int alive, idx, req, req_idx, l0, bank_e, par_e, hit_at, held, frozen, unfreezes;
    int asked_busy;   /* a request while he was busy (never allowed) */
} Pup;
static Pup P;
static void pup_tick(int now)
{
    if (!P.alive) return;
    if (P.hit_at && now == P.hit_at) P.idx = 37, P.l0 = 0x0B01, P.req = 0;   /* an enemy's blow: the engine's own damage state */
    if (P.idx == 37 && now >= P.hit_at + 20) P.idx = 5, P.l0 = 0x0001;
    if (P.req) {
        P.req = 0;
        if (P.frozen && P.req_idx == 5) return;   /* the stock freeze keeps him in EventWaitState: a MoveState request does not end it (live s22p3) */
        P.idx = P.req_idx;
        P.l0 = P.idx == 0 ? 0x0401 : 0x0001;
    }
}

/* ---- the partner's human (the other PC): an interaction from t0 for dur ticks with the motion id (and its object list in bank 0xE) */
typedef struct { int tag, t0, dur, id, src_res, src_i; } Act;
static Act act;
static int scene_hold;   /* a cutscene on this PC */

/* ---- the line */
#define QN 256
static struct { uint8_t b[PM_LEN]; int at; } q[QN];
static int nq, now, delay = 3, mirror_on = 1;
static pm_tx T;
static pm_rx R;
static int obj_list[2] = {1, 2};   /* the lever's two lists on this PC: resources 1 / 2 */
static int obj_loaded = 1;

static void step(int n)
{
    for (int i = 0; i < n; i++) {
        now++;
        /* the partner's PC: its human */
        int in = now >= act.t0 && now < act.t0 + act.dur;
        pm_layer l0 = {(uint16_t)(in ? act.id : 0x0001), 0, (float)(in ? now - act.t0 : 0), 1.0f}, l1 = {0x0501, 0, 0, 1.0f};
        pm_msg src, m;
        memset(&src, 0, sizeof src);
        if (in && pm_needs_bank((uint16_t)act.id)) src.has_src = 1, src.src_i = (uint8_t)act.src_i, src.src_params = 1, src.k4[0] = 7;
        if (pm_tx_step(&T, in ? act.tag : -1, &l0, &l1, &src, (uint32_t)now * 33u, &m) && mirror_on && nq < QN) pm_enc(q[nq].b, &m), q[nq].at = now + delay, nq++;
        /* this PC: words in */
        for (int k = 0; k < nq;) {
            if (q[k].at <= now) {
                pm_msg d;
                if (pm_dec(q[k].b, PM_LEN, &d)) pm_rx_word(&R, &d, (uint32_t)now * 33u);
                q[k] = q[--nq];
            } else k++;
        }
        /* this PC: the glue's pass */
        int pidx = P.alive ? P.idx : -1, hold = scene_hold || P.held;
        int a = pm_rx_step_f(&R, pidx, hold, P.frozen && P.idx == 0, (uint32_t)now * 33u);
        if (a & PM_A_UNFREEZE) { P.unfreezes++; if (P.frozen) P.frozen = 0, P.idx = 5, P.l0 = 0x0001; }   /* FUN_006AE390: the bits go, EventWaitState ends */
        if (a && hold && (a & ~PM_A_DROP)) P.asked_busy += 100;                       /* no call at all under a hold */
        if (a & PM_A_REQ_FREEZE) { if (!pm_idx_free(P.idx)) P.asked_busy++; P.req = 1, P.req_idx = 0; }
        if (a & PM_A_UNBIND) res_rel(P.bank_e), P.bank_e = 0, res_rel(P.par_e), P.par_e = 0;
        if (a & PM_A_BIND) {
            int lst = obj_loaded ? obj_list[R.cur.src_i] : 0;
            if (lst) res_rel(P.bank_e), res_add(lst), P.bank_e = lst; else R.bound = 0;
        }
        if (a & PM_A_MOTION) {
            if (!pm_needs_bank(R.cur.l0.id) || P.bank_e) P.l0 = R.cur.l0.id;
        }
        if (a & PM_A_REQ_RELEASE) P.req = 1, P.req_idx = 5;
        pup_tick(now);
    }
}
static void setup(void)
{
    memset(&P, 0, sizeof P), P.alive = 1, P.idx = 5, P.l0 = 0x0001;
    memset(ref, 0, sizeof ref), memset(freed, 0, sizeof freed), uaf = 0;
    ref[1] = ref[2] = 1, obj_loaded = 1;   /* the lever holds its own lists */
    pm_tx_reset(&T), pm_rx_reset(&R);
    nq = 0, now = 0, scene_hold = 0;
    memset(&act, 0, sizeof act), act.t0 = 1000000;
}
static int shows_motion(int id, int from, int to)   /* the puppet's layer 0 is id in every tick of [from, to) */
{
    for (int t = now; t < from; t++) step(1);
    int ok = 1;
    for (int t = from; t < to; t++) { step(1); ok &= P.l0 == id; }
    return ok;
}

static int scenarios(void)
{
    int f0 = fails;
    /* 1. a pickup (standard bank): shown while it runs, MoveState after */
    setup();
    act = (Act){PM_T_PICKUP, 10, 45, 0x0047, 0, 0};
    CHECK(shows_motion(0x0047, 20, 50));
    step(60);
    CHECK(P.idx == 5 && R.st == PM_R_IDLE && P.asked_busy == 0 && uaf == 0);
    /* 2. a lever pull (bank 0xE from the lever's list 2): bound while it runs, released after; refs balanced */
    setup();
    act = (Act){PM_T_LEVERON, 10, 60, 0x0E03, 2, 1};
    CHECK(shows_motion(0x0E03, 22, 65));
    CHECK(ref[2] == 2);                                  /* the lever's own + the puppet's */
    step(80);
    CHECK(P.idx == 5 && ref[2] == 1 && ref[1] == 1 && P.bank_e == 0 && uaf == 0);
    /* 3. the lever's stage is unloaded while the puppet holds its list, then the puppet despawns: no use after free, all refs end at 0 */
    setup();
    act = (Act){PM_T_LEVERON, 10, 200, 0x0E03, 1, 0};
    step(40);
    CHECK(P.l0 == 0x0E03 && ref[1] == 2);
    obj_loaded = 0, res_rel(1), res_rel(2);             /* the stage unload: the lever drops its own references */
    CHECK(ref[1] == 1 && !freed[1] && freed[2]);         /* list 1 lives on through the puppet's reference */
    pm_rx_drop(&R);                                       /* the glue: a new puppet unit / level instance: no call on the old unit */
    P.alive = 0, res_rel(P.bank_e), P.bank_e = 0;         /* the engine's despawn releases the puppet's banks */
    CHECK(ref[1] == 0 && freed[1] && uaf == 0);
    /* 4. the partner's object is not here (another area): a 0x0Exx motion is not played, no crash, released after */
    setup();
    obj_loaded = 0;
    act = (Act){PM_T_LEVEROFF, 10, 40, 0x0E02, 1, 0};
    step(30);
    CHECK(P.l0 != 0x0E02 && P.bank_e == 0 && R.st == PM_R_HOLD);
    step(60);
    CHECK(P.idx == 5 && uaf == 0);
    /* 5. a cutscene starts while the puppet is held: no call while it runs; the owed EventWaitState is released after it (the event ended) */
    setup();
    act = (Act){PM_T_DOOROPEN, 10, 40, 0x0071, 0, 0};
    step(30);
    CHECK(P.idx == 0 && P.l0 == 0x0071);
    scene_hold = 1;
    step(60);
    CHECK(P.asked_busy == 0 && P.idx == 0);              /* left alone while the scene runs */
    scene_hold = 0;
    step(30);
    CHECK(P.idx == 5 && R.st == PM_R_IDLE);
    /* 6. a hit takes the puppet during the event: the mirror lets go (its bank released), the engine's state stays, no request while he is busy */
    setup();
    act = (Act){PM_T_LEVERON, 10, 80, 0x0E03, 2, 1};
    P.hit_at = 40;
    step(45);
    CHECK(P.idx == 37 && P.bank_e == 0 && ref[2] == 1);
    step(60);
    CHECK(P.asked_busy == 0 && uaf == 0);
    /* 7. a busy puppet (a hit before the event) is never asked */
    setup();
    P.hit_at = 5;
    act = (Act){PM_T_PICKUP, 8, 10, 0x0047, 0, 0};
    step(60);
    CHECK(P.asked_busy == 0 && R.refused_busy > 0);
    /* 8. the partner's PC goes silent mid-event (the link): released after PM_SILENT_MS */
    setup();
    act = (Act){PM_T_DG_MOVE, 10, 400, 0x0E05, 1, 0};
    step(30);
    CHECK(P.idx == 0);
    mirror_on = 0;
    step((int)(PM_SILENT_MS / 33u) + 30);
    mirror_on = 1;
    CHECK(P.idx == 5 && R.silent == 1 && P.bank_e == 0 && ref[1] == 1);
    /* 9. two events back to back (knob then opening), and a KEEP does not restart the motion */
    setup();
    act = (Act){PM_T_KNOB, 10, 30, 0x006F, 0, 0};
    step(45);
    act = (Act){PM_T_DOOROPEN, 46, 40, 0x0071, 0, 0};
    CHECK(shows_motion(0x0071, 60, 80));
    step(80);
    CHECK(P.idx == 5 && R.events == 2);
    /* 10. live s22p1: the partner picks up while his puppet here stands in the standing wait (ExWaitState): shown, MoveState after */
    setup();
    P.idx = 2;
    act = (Act){PM_T_PICKUP, 10, 45, 0x004B, 0, 0};
    CHECK(shows_motion(0x004B, 20, 50));
    step(60);
    CHECK(P.idx == 5 && R.st == PM_R_IDLE && P.asked_busy == 0 && R.refused_busy == 0);
    /* 11. a turn (AboutFaceState, 6) is not a free state: never asked while it lasts */
    setup();
    P.idx = 6;
    act = (Act){PM_T_PICKUP, 10, 20, 0x004B, 0, 0};
    step(60);
    CHECK(P.idx == 6 && P.asked_busy == 0 && R.refused_busy > 0 && R.freezes == 0);
    CHECK(pm_idx_free(5) && pm_idx_free(2) && pm_idx_free(3) && pm_idx_free(4) && !pm_idx_free(0) && !pm_idx_free(1) && !pm_idx_free(6) && !pm_idx_free(37) && !pm_idx_free(98) &&
          !pm_idx_free(-1));
    /* 11b. live s22p3: the partner's puppet idles in CustomWaitReadyState (3): the pickup is shown, MoveState after */
    setup();
    P.idx = 3;
    act = (Act){PM_T_PICKUP, 10, 45, 0x004B, 0, 0};
    CHECK(shows_motion(0x004B, 20, 50));
    step(60);
    CHECK(P.idx == 5 && P.asked_busy == 0 && R.refused_busy == 0);
    /* 12. live s22p2: the puppet already waits in an EventWaitState of this PC's own code (not ours) and nothing holds him: the mirror takes it (no request), shows the
     * pickup, releases him to MoveState after */
    setup();
    P.idx = 0, P.l0 = 0x0401;
    act = (Act){PM_T_PICKUP, 10, 45, 0x004B, 0, 0};
    CHECK(shows_motion(0x004B, 20, 50));
    step(60);
    CHECK(P.idx == 5 && R.st == PM_R_IDLE && R.adopted == 1 && R.freezes == 0 && P.asked_busy == 0);
    /* 13. the same under a hold here (a cutscene): left alone, he stays in that EventWaitState */
    setup();
    P.idx = 0, P.l0 = 0x0401, scene_hold = 1;
    act = (Act){PM_T_PICKUP, 10, 45, 0x004B, 0, 0};
    step(80);
    CHECK(P.idx == 0 && P.l0 == 0x0401 && R.adopted == 0 && P.asked_busy == 0);
    scene_hold = 0;
    /* 14. live s22p3: a stock freeze (the guest's door) holds the puppet with no event and no hold: released by the stock unfreeze after PM_STALE_MS, once */
    setup();
    P.idx = 0, P.l0 = 0x0401, P.frozen = 1;
    step((int)(PM_STALE_MS / 33u) - 5);
    CHECK(P.idx == 0 && P.unfreezes == 0);
    step(20);
    CHECK(P.idx == 5 && P.unfreezes == 1 && !P.frozen);
    step(200);
    CHECK(P.unfreezes == 1);
    /* 15. the same freeze while the partner picks something up: adopted, shown, and the release brings the stock unfreeze (a MoveState request alone does not) */
    setup();
    P.idx = 0, P.l0 = 0x0401, P.frozen = 1;
    act = (Act){PM_T_PICKUP, 10, 45, 0x004B, 0, 0};
    CHECK(shows_motion(0x004B, 20, 50));
    step(60);
    CHECK(P.idx == 5 && !P.frozen && R.adopted == 1 && P.unfreezes == 1);
    /* 16. the stock freeze under a hold here (a cutscene) is left alone */
    setup();
    P.idx = 0, P.l0 = 0x0401, P.frozen = 1, scene_hold = 1;
    step(200);
    CHECK(P.idx == 0 && P.frozen && P.unfreezes == 0);
    scene_hold = 0;
    return fails - f0;
}

/* the build facts pmot1_game.inc relies on (work\image\rerev-image.bin, VA = 0x400000 + offset) */
static void image_facts(void)
{
    FILE *f = fopen("../../image/rerev-image.bin", "rb");
    if (!f) {
        printf("image not found: pmot1 build facts not checked\n");
        return;
    }
    static uint8_t img[0xE80000];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
#define AT(va) (img + ((va) - 0x400000u))
    static const uint8_t b1[] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x8B, 0xF1, 0x8B, 0x8C, 0x9E, 0x24, 0x0D, 0x00, 0x00};   /* FUN_00A2D7E0: [esi + bank*4 + 0xD24] */
    static const uint8_t b2[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0xF1, 0x83, 0xFF, 0x10};                           /* FUN_006B06F0: bank < 16 */
    CHECK(n > 0xD40000 && !memcmp(AT(0xA2D7E0u), b1, sizeof b1) && !memcmp(AT(0x6B06F0u), b2, sizeof b2));
    uint32_t v;
    memcpy(&v, AT(0x1065EA0u + 0x6C), 4);
    CHECK(v == 0x6B8CB0u);                                                         /* uPlayer's slot 27 = the setter the event states call through slot 26 */
    uint32_t e0, e5;
    memcpy(&e0, AT(0x1137498u), 4), memcpy(&e5, AT(0x1137498u + 5 * 4), 4);
    CHECK(e0 == 0x0117A670u && e5 == 0x0117A834u);                                /* the player's lower table: 0 EventWaitState, 5 MoveState */
    {   /* [2] = ExWaitState's DTI: its static constructor names it (mov ecx, 0x117A894 near push "ExWaitState") */
        uint32_t e2;
        memcpy(&e2, AT(0x1137498u + 2 * 4), 4);
        CHECK(e2 == 0x0117A894u);
        const uint8_t pat[5] = {0xB9, 0x94, 0xA8, 0x17, 0x01};
        int named = 0;
        for (size_t o = 0; o + 64 < n && !named; o++) {
            if (memcmp(img + o, pat, 5)) continue;
            for (size_t k = o > 20 ? o - 20 : 0; k < o + 20 && !named; k++) {
                if (img[k] != 0x68) continue;
                uint32_t v;
                memcpy(&v, img + k + 1, 4);
                if (v > 0x400000u && v - 0x400000u + 12 < n && !memcmp(img + (v - 0x400000u), "ExWaitState", 12)) named = 1;
            }
        }
        CHECK(named);
    }
    static const uint8_t lever_bind[] = {0x8B, 0x84, 0xAE, 0x28, 0x0D, 0x00, 0x00, 0x6A, 0x0E, 0x50, 0x8B, 0xCB, 0xE8};   /* 0x643A89: mov eax,[esi+ebp*4+0xD28]; push 0xE; push eax; mov ecx,ebx; call */
    CHECK(!memcmp(AT(0x643A89u), lever_bind, sizeof lever_bind));
    printf("image: the two bank binders, uPlayer's motion setter, the lower table's indices 0 / 5 and the lever's bank-0xE binding checked\n");
#undef AT
}

int main(void)
{
    image_facts();
    /* the wire */
    pm_msg m = {PM_MOT, 4, 9, PM_T_PRESS_LOOP, {0x0E04, 2.5f, 12.0f, 1.0f}, {0x0501, 0, 3.0f, 1.0f}, 1, 1, 1, {0x10, 0x20, 3, 300}}, d;
    uint8_t b[PM_LEN];
    CHECK(pm_enc(b, &m) == PM_LEN && pm_dec(b, PM_LEN, &d) && d.kind == PM_MOT && d.ev == 4 && d.seq == 9 && d.tag == PM_T_PRESS_LOOP && d.l0.id == 0x0E04 && d.l0.frame == 12.0f &&
          d.has_src && d.src_i == 1 && d.src_params && d.k4[3] == 300);
    CHECK(!pm_dec(b, PM_LEN - 1, &d));
    b[4] = PM_T_N;
    CHECK(!pm_dec(b, PM_LEN, &d));
    pm_enc(b, &m), b[34] = 2;
    CHECK(!pm_dec(b, PM_LEN, &d));
    m.l0.frame = 1.0e9f, pm_enc(b, &m);
    CHECK(!pm_dec(b, PM_LEN, &d));
    CHECK(pm_needs_bank(0x0E03) && !pm_needs_bank(0x0047) && !pm_needs_bank(0xFFFF));
    /* the sender: BEGIN, a change, KEEPs, END */
    pm_tx t;
    pm_tx_reset(&t);
    pm_layer a = {0x0047, 0, 0, 1}, z = {0x0501, 0, 0, 1};
    pm_msg o;
    CHECK(!pm_tx_step(&t, -1, &a, &z, NULL, 0, &o));
    CHECK(pm_tx_step(&t, PM_T_PICKUP, &a, &z, NULL, 10, &o) && o.kind == PM_BEGIN && o.ev == 1);
    CHECK(!pm_tx_step(&t, PM_T_PICKUP, &a, &z, NULL, 100, &o));
    CHECK(pm_tx_step(&t, PM_T_PICKUP, &a, &z, NULL, 10 + PM_KEEP_MS, &o) && o.kind == PM_KEEP);
    a.id = 0x004B;
    CHECK(pm_tx_step(&t, PM_T_PICKUP, &a, &z, NULL, 400, &o) && o.kind == PM_MOT && o.l0.id == 0x004B);
    CHECK(pm_tx_step(&t, -1, &a, &z, NULL, 450, &o) && o.kind == PM_END && o.ev == 1);
    CHECK(pm_tx_step(&t, PM_T_KNOB, &a, &z, NULL, 500, &o) && o.kind == PM_BEGIN && o.ev == 2);

    int s = scenarios();
    CHECK(s == 0);
    /* the model before the fix (0.2.1): no mirror, the puppet never shows the partner's pickup */
    mirror_on = 0;
    int f0 = fails;
    setup();
    act = (Act){PM_T_PICKUP, 10, 45, 0x0047, 0, 0};
    int shown = shows_motion(0x0047, 20, 50);
    printf("MODEL BEFORE THE FIX: the partner picks something up, his puppet here shows the pickup motion: %s\n", shown ? "yes (?!)" : "no (MODEL BEFORE THE FIX FAILS)");
    CHECK(!shown);
    fails = f0 + (shown ? 1 : 0);
    mirror_on = 1;
    printf("test_pmot1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
