/*
 * objsync1_game.inc - included by world1_game.c after motion1_game.inc: sync022 row W1 (objsync1.h), a lever the partner pulls moves here too.
 *   both PCs, logic thread, model lock: every 100 ms the object list +0xBC is scanned for the two uLeverBase classes; a move (OffMove / OnMove) started by THIS
 *   PC's human (state+0x10 == the own slot) is announced (ext CP_W1_OBJ); the other PC, when its copy still waits in the state the operator left, asks its
 *   copy's manager for the same move with operator -1 through the deferred request by index FUN_0049C790 (applied by the copy's own tick): the copy plays its
 *   own pull, its own end and sets its own scenario flag (FUN_00645540(+0xF18, v)). Every move end is logged with the lever's flag value, on both PCs.
 * `world set levers 0|1` (default 1), `world objs` = status.
 */
#include "objsync1.h"

#define OS_REQ_FN 0x49C790u     /* FUN_0049C790: cCharaStateManager request by index, thiscall(mgr; index, arg, PointerArgs*) ret 0xC */
#define OS_VT_WALL 0x103C140u   /* uOMCommonLeverWall class vtable (vtables-rev1.txt) */
#define OS_VT_FLOOR 0x103BE10u  /* uOMCommonLeverFloor */
#define OS_U_FLAG 0xF18u        /* the lever's scenario flag (FUN_00645540(param_2[0x3C6], v)), -1 = none */

static volatile int f_levers = 1;
static os_t OSY;
static uint32_t os_last_ms;
static int os_req_ok = -1;                  /* -1 not checked, 0 refused, 1 the bytes of FUN_0049C790 are the expected ones */
static uint32_t os_ids[OS_NSTATES];         /* the DTI ids of the 5 lever states (0 = not verified) */
static int os_ids_done;
static struct { uint32_t k4[4]; int used, idx, op; } os_lg[OS_MAX];   /* the log of move ends: the last index / operator per lever */
static struct { unsigned passes, levers, sent, send_fail, rx, bad, replays, req_refused, nocopy, wrongclass, waits, ends_own, ends_replay, ends_other, lines; } OSS;
typedef unsigned (TC *osreq_fn)(void *mgr, int index, int arg, void *args);

static const uint32_t os_dti[OS_NSTATES] = {0x01176AD4u, 0x01176B14u, 0x01176B54u, 0x01176BA0u, 0x01176AF4u};   /* dti-rev1.txt, table 0x11303DC */
static const char *const os_name[OS_NSTATES] = {"OffWaitState", "OffMoveState", "OnWaitState", "OnMoveState", "EndWaitState"};

static void os_resolve(void)
{
    if (os_ids_done) return;
    int all = 1;
    for (int i = 0; i < OS_NSTATES; i++) {
        uint32_t vt = rd32(os_dti[i]), nm = rd32(os_dti[i] + 4), id = rd32(os_dti[i] + 0x1C);
        char s[16] = "";
        if (nm) MEM.read(MEM.user, nm, s, sizeof s - 1);
        if (vt >= WH_DTI_VT_LO && vt < WH_DTI_VT_HI && id && !strcmp(s, os_name[i])) os_ids[i] = id;
        else os_ids[i] = 0, all = 0;
    }
    /* the request's prologue: sub esp,0x378; push esi; mov esi,ecx; cmp byte [esi+0x2d],1; jne */
    static const uint8_t sig[] = {0x81, 0xEC, 0x78, 0x03, 0x00, 0x00, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x2D, 0x01, 0x75, 0x0C};
    uint8_t cur[sizeof sig];
    os_req_ok = MEM.read(MEM.user, OS_REQ_FN, cur, sizeof cur) && !memcmp(cur, sig, sizeof sig);
    os_ids_done = 1;
    wlog("levers: state DTIs %s (%08x %08x %08x %08x %08x), the request call 0x49C790 %s", all ? "verified" : "NOT all verified (0 = refused)", (unsigned)os_ids[0], (unsigned)os_ids[1],
         (unsigned)os_ids[2], (unsigned)os_ids[3], (unsigned)os_ids[4], os_req_ok ? "verified" : "REFUSED (other bytes)");
}

/* the lever index of a unit: its class / vtable, its manager's current index and the current state's DTI id must agree; *op = state+0x10 for the moves */
static int os_lever_state(uint32_t unit, uint32_t *cls_out, int *op)
{
    uint32_t cls = wh_unit_class(&MEM, unit), vt = rd32(unit);
    *op = -1;
    if (!os_lever_class(cls) || (vt != OS_VT_WALL && vt != OS_VT_FLOOR)) return -2;
    if (cls_out) *cls_out = cls;
    uint32_t mgr = rd32(unit + WH_U_MACHINE), st = mgr ? rd32(mgr + 0x14) : 0;
    if (!mgr || !st) return -1;
    int idx = (int)rd32(mgr + 0x3C);
    uint32_t id = cur_state(unit);
    if (idx < 0 || idx >= OS_NSTATES || !id || os_ids[idx] != id) return -1;
    if (os_is_move(idx)) *op = (int)rd32(st + 0x10);
    return idx;
}

static int os_flag_now(uint32_t unit, int *fid)
{
    *fid = (int)rd32(unit + OS_U_FLAG);
    return *fid >= 0 ? flag_get((uint32_t)*fid) : -9;
}

/* logic thread: the move ends seen here, with the lever's own flag (the proof that the copy's own end ran) */
static void os_note(uint32_t unit, const uint32_t k4[4], int idx, int op)
{
    int slot = -1, fr = -1;
    for (int i = 0; i < OS_MAX; i++) {
        if (os_lg[i].used && !memcmp(os_lg[i].k4, k4, 16)) { slot = i; break; }
        if (!os_lg[i].used && fr < 0) fr = i;
    }
    if (slot < 0) {
        if (fr < 0) return;
        slot = fr, os_lg[slot].used = 1, memcpy(os_lg[slot].k4, k4, 16), os_lg[slot].idx = idx, os_lg[slot].op = op;
        return;
    }
    int pidx = os_lg[slot].idx, pop = os_lg[slot].op;
    os_lg[slot].idx = idx, os_lg[slot].op = op;
    if (pidx == idx || !os_is_move(pidx)) return;   /* only the end of a move is logged */
    int fid = -1, v = os_flag_now(unit, &fid);
    const char *by = pop == g_own ? "this PC's own human" : pop == -1 ? "operator -1 (a replay of the partner's pull, its flag or an auto-return)" : "another slot";
    if (pop == g_own) OSS.ends_own++;
    else if (pop == -1) OSS.ends_replay++;
    else OSS.ends_other++;
    if (++OSS.lines <= 200)
        wlog("levers: lever %08x key %08x/%08x/%u/%u: %s ended (%s) -> %s; its scenario flag %d = %d here", (unsigned)unit, (unsigned)k4[0], (unsigned)k4[1], (unsigned)k4[2], (unsigned)k4[3],
             os_name[pidx], by, idx >= 0 && idx < OS_NSTATES ? os_name[idx] : "?", fid, v);
}

static void os_try(const os_msg *m, int retry, uint32_t now)
{
    uint32_t sa = sarrange();
    wh_entry e;
    if (!sa || !wh_find_key4(&MEM, sa, WH_LIST_OBJECTS, m->k4, &e) || !e.unit) {
        OSS.nocopy++;
        if (OSS.lines++ < 200) wlog("levers: the partner pulled lever %08x/%08x/%u/%u (%s): no copy here (not placed / another area): nothing to do", (unsigned)m->k4[0], (unsigned)m->k4[1],
                                    (unsigned)m->k4[2], (unsigned)m->k4[3], os_name[m->to]);
        return;
    }
    uint32_t cls = 0;
    int op = -1, cur = os_lever_state(e.unit, &cls, &op);
    if (cur == -2 || cls != m->cls) {
        OSS.wrongclass++;
        if (OSS.lines++ < 200) wlog("levers: key %08x/%08x/%u/%u is not the same lever class here (class %08x, vtable %08x): left alone", (unsigned)m->k4[0], (unsigned)m->k4[1], (unsigned)m->k4[2],
                                    (unsigned)m->k4[3], (unsigned)wh_unit_class(&MEM, e.unit), (unsigned)rd32(e.unit));
        return;
    }
    int d = os_rx_decide(&OSY, m, cur, op, retry);
    if (d == OS_WAIT) {
        os_wait_keep(&OSY, m, now);
        OSS.waits++;
        return;
    }
    if (d != OS_REPLAY) {
        if (d != OS_SKIP_OLD && OSS.lines++ < 200)
            wlog("levers: the partner pulled lever %08x key %08x/%08x/%u/%u (%s -> %s); here it is %s (operator %d): %s", (unsigned)e.unit, (unsigned)m->k4[0], (unsigned)m->k4[1], (unsigned)m->k4[2],
                 (unsigned)m->k4[3], os_name[m->from], os_name[m->to], cur >= 0 ? os_name[cur] : "in an unknown state", op, d == OS_ALREADY ? "already there, nothing to do" : "left alone");
        return;
    }
    uint32_t mgr = rd32(e.unit + WH_U_MACHINE);
    if (!os_req_ok || !mgr || rd8(mgr + 0x2D) == 1) {
        OSS.req_refused++;
        if (OSS.lines++ < 200) wlog("levers: lever %08x: the replay is REFUSED (request call %s, manager %08x)", (unsigned)e.unit, os_req_ok ? "ok" : "not verified", (unsigned)mgr);
        return;
    }
    ops_enter();
    unsigned r = ((osreq_fn)(uintptr_t)OS_REQ_FN)((void *)(uintptr_t)mgr, m->to, -1, NULL);
    ops_leave();
    OSS.replays++;
    if (OSS.lines++ < 200)
        wlog("levers: the partner pulled lever %08x key %08x/%08x/%u/%u (%s -> %s): the copy here asks for %s with operator -1 (request %s; the copy's own tick plays it, no player touched)",
             (unsigned)e.unit, (unsigned)m->k4[0], (unsigned)m->k4[1], (unsigned)m->k4[2], (unsigned)m->k4[3], os_name[m->from], os_name[m->to], os_name[m->to], (r & 0xFF) ? "taken" : "REFUSED by the manager (a request of higher priority waits)");
}

/* the receiver (op_ext, logic thread, model lock) */
static void lever_rx(const uint8_t *b, unsigned len)
{
    os_msg m;
    OSS.rx++;
    if (!os_dec(b, len, &m)) {
        OSS.bad++;
        return;
    }
    if (!f_levers) return;
    os_resolve();
    os_try(&m, 0, cs_now(bound_cs));
}

/* the sender + the waiting announces (both PCs, logic thread, model lock) */
static void lever_pass(uint32_t now)
{
    if (!f_levers || (int32_t)(now - os_last_ms) < 100) return;
    os_last_ms = now;
    os_resolve();
    uint32_t sa = sarrange();
    if (!sa || !call_ok(WH_C_CURSTATE)) return;
    OSS.passes++;
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_OBJECTS), lev = 0;
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_OBJECTS, i, &e) || !e.unit || !os_lever_class(wh_unit_class(&MEM, e.unit))) continue;
        uint32_t cls = 0;
        int op = -1, idx = os_lever_state(e.unit, &cls, &op);
        if (idx < 0) continue;
        lev++;
        os_note(e.unit, e.k, idx, op);
        os_msg m;
        if (os_tx_step(&OSY, e.k, cls, idx, op, g_own, &m)) {
            uint8_t buf[OS_LEN];
            os_enc(buf, &m);
            int ok = w1_send_ext(&N.w, CP_W1_OBJ, buf, OS_LEN);
            if (ok) OSS.sent++;
            else OSS.send_fail++;
            if (OSS.lines++ < 200)
                wlog("levers: lever %08x key %08x/%08x/%u/%u: this PC's own human started %s (from %s): %s to the partner (#%u)", (unsigned)e.unit, (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2],
                     (unsigned)e.k[3], os_name[m.to], os_name[m.from], ok ? "announced" : "NOT announced", m.seq);
        }
    }
    OSS.levers = lev;
    unsigned it = 0;
    os_msg w;
    while (os_wait_next(&OSY, now, &w, &it)) os_try(&w, 1, now);
}

static void lever_reset(void)
{
    os_reset(&OSY);
    memset(os_lg, 0, sizeof os_lg);
}

static void lever_status(char *out, int size)
{
    int o = snprintf(out, (size_t)size,
             "world objs (objsync1.h, levers): levers %d | request call %s, state DTIs %s | scan passes %u, levers now %u | sent %u (not queued %u), moves seen %u (not this PC's human %u) | "
             "received %u (bad %u, old %u): replayed %u, already there %u, left alone %u, waited %u (expired %u), no copy %u, other class %u, refused %u | move ends: own %u, operator -1 %u, other %u",
             f_levers, os_req_ok < 0 ? "not checked" : os_req_ok ? "verified" : "REFUSED", os_ids[0] && os_ids[1] && os_ids[2] && os_ids[3] && os_ids[4] ? "verified" : "not all verified", OSS.passes,
             OSS.levers, OSS.sent, OSS.send_fail, OSY.seen_moves, OSY.not_own, OSS.rx, OSS.bad, OSY.old, OSY.replayed, OSY.already, OSY.other, OSY.waited, OSY.wait_expired, OSS.nocopy, OSS.wrongclass,
             OSS.req_refused, OSS.ends_own, OSS.ends_replay, OSS.ends_other);
    /* the levers of the list (lab: where to stand): unit, key, position, distance to this PC's human, state, operator, the flag */
    os_resolve();
    uint32_t sa = sarrange(), me = 0;
    wh_player_by_slot(&MEM, (unsigned)g_own, &me);
    unsigned n = sa ? wh_list_count(&MEM, sa, WH_LIST_OBJECTS) : 0, shown = 0;
    for (unsigned i = 0; i < n && o > 0 && o < size - 220 && shown < 12; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_OBJECTS, i, &e) || !e.unit || !os_lever_class(wh_unit_class(&MEM, e.unit))) continue;
        uint32_t cls = 0;
        int op = -1, idx = os_lever_state(e.unit, &cls, &op), fid = -1, fv = os_flag_now(e.unit, &fid);
        float dx = me ? rdf(e.unit + WH_U_POS) - rdf(me + WH_U_POS) : 0, dz = me ? rdf(e.unit + WH_U_POS + 8) - rdf(me + WH_U_POS + 8) : 0;
        o += snprintf(out + o, (size_t)(size - o), "\n  lever %08x class %08x key %08x/%08x/%u/%u pos (%.0f,%.0f,%.0f) dist %.0f state %s op %d type %d flag %d = %d", (unsigned)e.unit, (unsigned)cls,
                      (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2], (unsigned)e.k[3], (double)rdf(e.unit + WH_U_POS), (double)rdf(e.unit + WH_U_POS + 4), (double)rdf(e.unit + WH_U_POS + 8),
                      (double)sqrtf(dx * dx + dz * dz), idx >= 0 && idx < OS_NSTATES ? os_name[idx] : "?", op, (int)rd32(e.unit + 0x110Cu), fid, fv);
        shown++;
    }
}
