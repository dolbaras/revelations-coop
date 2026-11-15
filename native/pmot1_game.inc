/*
 * pmot1_game.inc - included by world1_game.c after objsync1_game.inc: sync022 rows P5-P10 / P12 (pmot1.h), the partner's interaction motions on his puppet here.
 *   sender (this PC's own human, ~30 Hz, logic thread): its lower state (unit+0xE80, 0x49C3E0) is one of the verified interaction DTIs -> BEGIN / MOT / KEEP / END
 *   with layer 0 / 1 and, for a bank-0xE motion, the key4 of the object whose list the human holds in bank 0xE (ext CP_W1_PMOT);
 *   receiver (the puppet = the human of the other slot): pmot1.h's machine; the hold and the release are deferred requests by index (FUN_0049C790 0 = EventWaitState,
 *   5 = MoveState), the bank binding is the object's own call (FUN_00A2D7E0 / FUN_006B06F0 on the puppet with ITS copy's lists), the motion the unit's own
 *   setter (vt+0x6C = 0x6B8CB0). Every call is on this logic thread, after the reads that prove the unit, its vtable, its manager and its state.
 * `world set pmot 0|1` (default 1), `world set pmtrav 0|1` (traversal states, default 0), `world pmot` = status.
 */
#include "pmot1.h"

#define PM_FN_BIND 0xA2D7E0u      /* FUN_00A2D7E0 thiscall(unit; list, bank) ret 8: release the old list of the bank, add-ref the new one */
#define PM_FN_PARAM 0x6B06F0u     /* FUN_006B06F0 thiscall(player; params, bank) ret 8: the same for player+0x13B8 + bank*8 */
#define PM_FN_SETMOT 0x6B8CB0u    /* uPlayer vtable slot 27 (+0x6C): (layer, id, blend, frame, speed, flags) */
#define PM_FN_UNFREEZE 0x6AE390u   /* the stock release of FUN_006B3B80's freeze: thiscall(unit) plain ret: clears +0xE1C bit 2 / +0xF44 bit 3 (EventWaitState then ends) */
#define PM_DTI_EVWAIT 0x0117A670u
#define PM_DTI_MOVE 0x0117A834u

int puppet_scene_now(unsigned *flags);       /* puppet_game.c: this PC's own scene flags (CP_SF_LOADING / CUTSCENE / PAUSED), 1 = fresh */
int puppet_partner_flags(unsigned *flags);   /* puppet_game.c: the partner's newest pad / STATE flags (CP_SF_*), 1 = fresh */
static volatile int f_pmot = 1, f_pmtrav = 0;
static int pm_par_bound;                     /* we bound the puppet's bank-0xE params too (released with the list) */
static pm_tx PMT;
static pm_rx PMR;
static uint32_t pm_ids[PM_T_N], pm_id_evwait, pm_id_move, pm_id_exwait, pm_id_cwready, pm_id_cwait;
static unsigned pm_busy_ev = 0x100;           /* the partner's event whose busy refusal was logged */
#define PM_LOWER_TABLE 0x1137498u           /* the player's lower state table: [index] = the state's DTI (0 EventWaitState, 2 ExWaitState, 5 MoveState ...) */
static int pm_ids_done, pm_fn_ok = -1, pm_unf_ok = -1;
static uint32_t pm_last_ms, pm_unit;      /* the puppet unit the receiver's state belongs to */
static struct { unsigned tx, tx_fail, rx, bad, reqs, req_refused, binds, bind_refused, params, motions, mot_refused, drops, lines, unfreezes; } PMS;
typedef void (TC *pm_bind_fn)(void *unit, uint32_t res, unsigned bank);

static void pm_resolve(void)
{
    if (pm_ids_done) return;
    int all = 1;
    for (int i = 0; i < PM_T_N; i++) {
        uint32_t vt = rd32(pm_states[i].dti), nm = rd32(pm_states[i].dti + 4), id = rd32(pm_states[i].dti + 0x1C);
        char s[40] = "";
        if (nm) MEM.read(MEM.user, nm, s, sizeof s - 1);
        pm_ids[i] = vt >= WH_DTI_VT_LO && vt < WH_DTI_VT_HI && id && !strcmp(s, pm_states[i].name) ? id : 0;
        if (!pm_ids[i]) all = 0;
    }
    char s1[24] = "", s2[24] = "";
    uint32_t n1 = rd32(PM_DTI_EVWAIT + 4), n2 = rd32(PM_DTI_MOVE + 4);
    if (n1) MEM.read(MEM.user, n1, s1, sizeof s1 - 1);
    if (n2) MEM.read(MEM.user, n2, s2, sizeof s2 - 1);
    pm_id_evwait = !strcmp(s1, "EventWaitState") ? rd32(PM_DTI_EVWAIT + 0x1C) : 0;
    pm_id_move = !strcmp(s2, "MoveState") ? rd32(PM_DTI_MOVE + 0x1C) : 0;
    {   /* the standing waits: the table's entries 2 / 3 / 4 by their names */
        static const struct { int idx; const char *name; uint32_t *id; } w[3] = {{PM_IDX_EXWAIT, "ExWaitState", &pm_id_exwait},
                                                                               {PM_IDX_CWREADY, "CustomWaitReadyState", &pm_id_cwready},
                                                                               {PM_IDX_CWAIT, "CustomWaitState", &pm_id_cwait}};
        for (int i = 0; i < 3; i++) {
            uint32_t d = rd32(PM_LOWER_TABLE + (uint32_t)w[i].idx * 4u), n3 = d ? rd32(d + 4) : 0;
            char s3[28] = "";
            if (n3) MEM.read(MEM.user, n3, s3, sizeof s3 - 1);
            *w[i].id = !strcmp(s3, w[i].name) ? rd32(d + 0x1C) : 0;
        }
    }
    /* the prologues of the two binders */
    static const uint8_t b1[] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x8B, 0xF1, 0x8B, 0x8C, 0x9E, 0x24, 0x0D, 0x00, 0x00};
    static const uint8_t b2[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0xF1, 0x83, 0xFF, 0x10};
    uint8_t c1[sizeof b1], c2[sizeof b2];
    pm_fn_ok = MEM.read(MEM.user, PM_FN_BIND, c1, sizeof c1) && !memcmp(c1, b1, sizeof b1) && MEM.read(MEM.user, PM_FN_PARAM, c2, sizeof c2) && !memcmp(c2, b2, sizeof b2);
    {   /* push ebx; push esi; mov esi,ecx; mov ebx,[esi+0xe1c]; and dword [esi+0xe1c],0xfffffffb */
        static const uint8_t b3[] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x9E, 0x1C, 0x0E, 0x00, 0x00, 0x83, 0xA6, 0x1C, 0x0E, 0x00, 0x00, 0xFB};
        uint8_t c3[sizeof b3];
        pm_unf_ok = MEM.read(MEM.user, PM_FN_UNFREEZE, c3, sizeof c3) && !memcmp(c3, b3, sizeof b3);
    }
    pm_ids_done = 1;
    wlog("pmot: interaction state DTIs %s, EventWaitState %08x, MoveState %08x, ExWaitState %08x, the bank binders %s, the request call %s", all ? "verified" : "NOT all verified (0 = never mirrored)",
         (unsigned)pm_id_evwait, (unsigned)pm_id_move, (unsigned)pm_id_exwait, pm_fn_ok ? "verified" : "REFUSED", os_req_ok > 0 ? "verified" : "not verified");
}

static int pm_tag_of(uint32_t id)
{
    if (!id) return -1;
    for (int i = 0; i < PM_T_N; i++)
        if (pm_ids[i] == id) return i >= PM_T_N_INTERACT && !f_pmtrav ? -1 : i;
    return -1;
}
static void pm_read_layer(uint32_t unit, unsigned l, pm_layer *o)
{
    uint32_t ly = unit + MO_LAYER_OFF + l * MO_LAYER_SIZE;
    o->id = (uint16_t)rd16(ly + 4), o->blend = rdf(ly + 0x40), o->frame = rdf(ly + 0x4C), o->speed = rdf(ly + 0x5C);
    if (!isfinite(o->blend) || o->blend < 0 || o->blend >= 1000.0f) o->blend = 0;
    if (!isfinite(o->frame) || fabsf(o->frame) > 1.0e6f) o->frame = 0;
    if (!isfinite(o->speed) || fabsf(o->speed) > 100.0f) o->speed = 1.0f;
}
/* the object whose bank-1 / bank-2 list (obj+0xD28 + i*4) is the human's bank 0xE list now */
static int pm_find_src(uint32_t unit, pm_msg *src)
{
    uint32_t list = rd32(unit + 0xD24u + PM_BANK_EVENT * 4u), par = rd32(unit + 0x13B8u + PM_BANK_EVENT * 8u), sa = sarrange();
    memset(src, 0, sizeof *src);
    if (!list || !sa) return 0;
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_OBJECTS);
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_OBJECTS, i, &e) || !e.unit) continue;
        for (unsigned k = 0; k < 2; k++) {
            if (rd32(e.unit + 0xD28u + k * 4u) != list) continue;
            src->has_src = 1, src->src_i = (uint8_t)k, src->src_params = par && rd32(e.unit + 0xF48u + k * 8u) == par;
            memcpy(src->k4, e.k, 16);
            return 1;
        }
    }
    return 0;
}

/* the puppet's lower state index, proven by its DTI id: 0 = our EventWaitState, 5 = MoveState, another index = busy, -1 = no puppet / unknown */
static int pm_puppet_idx(uint32_t unit)
{
    if (!unit || !wh_class_is_player(wh_unit_class(&MEM, unit)) || rd32(rd32(unit) + 0x6C) != PM_FN_SETMOT) return -1;
    uint32_t mgr = rd32(unit + WH_U_MACHINE);
    if (!mgr || !rd32(mgr + 0x14)) return -1;
    int idx = (int)rd32(mgr + 0x3C);
    uint32_t id = cur_state(unit);
    if (idx == PM_IDX_EVENTWAIT) return pm_id_evwait && id == pm_id_evwait ? 0 : -1;
    if (idx == PM_IDX_MOVE) return pm_id_move && id == pm_id_move ? 5 : -1;
    if (idx == PM_IDX_EXWAIT) return pm_id_exwait && id == pm_id_exwait ? PM_IDX_EXWAIT : 98;   /* not proven by its DTI: busy */
    if (idx == PM_IDX_CWREADY) return pm_id_cwready && id == pm_id_cwready ? PM_IDX_CWREADY : 98;
    if (idx == PM_IDX_CWAIT) return pm_id_cwait && id == pm_id_cwait ? PM_IDX_CWAIT : 98;
    return idx < 0 ? 99 : idx;   /* a hit, a grab, a scene state ... (or a state asked by DTI: index -1) */
}
/* the name of the puppet's lower state (read only: the manager's index -> the table's DTI name; for the busy log) */
static const char *pm_state_name(uint32_t unit, char *b, size_t cap)
{
    uint32_t mgr = rd32(unit + WH_U_MACHINE);
    int idx = mgr ? (int)rd32(mgr + 0x3C) : -1;
    snprintf(b, cap, "index %d", idx);
    if (idx < 0 || idx >= 64) return b;
    uint32_t d = rd32(PM_LOWER_TABLE + (uint32_t)idx * 4u), n = d ? rd32(d + 4) : 0;
    char s[32] = "";
    if (n && MEM.read(MEM.user, n, s, sizeof s - 1) && s[0]) {
        s[sizeof s - 1] = 0;
        snprintf(b, cap, "index %d %s", idx, s);
    }
    return b;
}

static int pm_request(uint32_t unit, int index, const char *why)
{
    uint32_t mgr = rd32(unit + WH_U_MACHINE);
    if (os_req_ok <= 0 || !mgr || rd8(mgr + 0x2D) == 1) {
        PMS.req_refused++;
        return 0;
    }
    ops_enter();
    unsigned r = ((osreq_fn)(uintptr_t)OS_REQ_FN)((void *)(uintptr_t)mgr, index, 0, NULL);
    ops_leave();
    PMS.reqs++;
    if (PMS.lines++ < 300) wlog("pmot: puppet %08x: %s -> asks for %s (request %s)", (unsigned)unit, why, index == PM_IDX_EVENTWAIT ? "EventWaitState" : "MoveState", (r & 0xFF) ? "taken" : "REFUSED by the manager");
    return (r & 0xFF) != 0;
}

static void pm_bind(uint32_t unit, int on)
{
    if (pm_fn_ok <= 0) {
        PMS.bind_refused++;
        return;
    }
    uint32_t list = 0, par = 0, sa = sarrange();
    int use_par = 0;
    if (on) {
        wh_entry e;
        if (!sa || !wh_find_key4(&MEM, sa, WH_LIST_OBJECTS, PMR.cur.k4, &e) || !e.unit || PMR.cur.src_i > 1) {
            PMS.bind_refused++;
            if (PMS.lines++ < 300) wlog("pmot: puppet %08x: the partner's object %08x/%08x/%u/%u is not here: no bank 0xE, the 0x0Exx motion is not played", (unsigned)unit, (unsigned)PMR.cur.k4[0],
                                        (unsigned)PMR.cur.k4[1], (unsigned)PMR.cur.k4[2], (unsigned)PMR.cur.k4[3]);
            PMR.bound = 0;
            return;
        }
        list = rd32(e.unit + 0xD28u + PMR.cur.src_i * 4u);
        par = rd32(e.unit + 0xF48u + PMR.cur.src_i * 8u);
        use_par = PMR.cur.src_params && par;
        if (!list) {
            PMS.bind_refused++, PMR.bound = 0;
            return;
        }
    } else {
        use_par = pm_par_bound;   /* release what we took (0 only releases) */
    }
    ops_enter();
    ((pm_bind_fn)(uintptr_t)PM_FN_BIND)((void *)(uintptr_t)unit, list, PM_BANK_EVENT);
    if (use_par) ((pm_bind_fn)(uintptr_t)PM_FN_PARAM)((void *)(uintptr_t)unit, on ? par : 0, PM_BANK_EVENT);
    ops_leave();
    PMS.binds++;
    if (use_par) PMS.params++;
    pm_par_bound = on && use_par;
    if (PMS.lines++ < 300) wlog("pmot: puppet %08x: bank 0xE %s (list %08x%s)", (unsigned)unit, on ? "bound to the partner's object's list here" : "released", (unsigned)list, use_par ? (on ? ", params too" : ", params too") : "");
}

static void pm_motion(uint32_t unit)
{
    const pm_layer *l = &PMR.cur.l0;
    uint32_t rec = unit + MO_LAYER_OFF;
    if (l->id == 0xFFFF || (pm_needs_bank(l->id) && !PMR.bound) || (rd32(unit + 0x40Cu) != 0 && rd32(unit + 0xD64u) > 0 && rd32(rec + 0x13Cu) == 0)) {
        PMS.mot_refused++;
        return;
    }
    uint32_t bb, sb;
    float sp = l->speed > 0 ? l->speed : 1.0f;
    memcpy(&bb, &l->blend, 4), memcpy(&sb, &sp, 4);
    ops_enter();
    ((setmot_fn)(uintptr_t)PM_FN_SETMOT)((void *)(uintptr_t)unit, 0, l->id, bb, l->frame, sb, 4);
    ops_leave();
    PMS.motions++;
    if (PMS.lines++ < 300)
        wlog("pmot: puppet %08x plays the partner's %s motion %04x from frame %.1f (speed %.2f)", (unsigned)unit, PMR.cur.tag < PM_T_N ? pm_states[PMR.cur.tag].name : "?", l->id, (double)l->frame, (double)sp);
}

/* the receiver (op_ext, logic thread, model lock) */
static void pmot_rx(const uint8_t *b, unsigned len)
{
    pm_msg m;
    PMS.rx++;
    if (!pm_dec(b, len, &m)) {
        PMS.bad++;
        return;
    }
    if (!f_pmot) return;
    pm_rx_word(&PMR, &m, cs_now(bound_cs));
}

static int pm_hold_now(uint32_t unit)
{
    unsigned sf = 0, pf = 0;
    if (puppet_scene_now(&sf) && (sf & (CP_SF_LOADING | CP_SF_CUTSCENE))) return 1;
    if (puppet_partner_flags(&pf) && (pf & (CP_SF_DEAD | CP_SF_DOWN | CP_SF_LOADING | CP_SF_CUTSCENE | CP_SF_HELD))) return 1;
    if (!(rdf(unit + WH_U_HP) > 0.0f) || (rd32(unit + WH_U_FLAGS) & 1u)) return 1;
    return 0;
}

static void pmot_pass(uint32_t now)
{
    if ((int32_t)(now - pm_last_ms) < 33) return;
    pm_last_ms = now;
    pm_resolve();
    if (!call_ok(WH_C_CURSTATE)) return;
    /* sender */
    uint32_t me = 0;
    if (f_pmot && wh_player_by_slot(&MEM, (unsigned)g_own, &me) && me) {
        int tag = pm_tag_of(cur_state(me));
        pm_layer l0, l1;
        pm_read_layer(me, 0, &l0), pm_read_layer(me, 1, &l1);
        pm_msg src, m;
        if (tag >= 0 && (pm_needs_bank(l0.id) || pm_needs_bank(l1.id))) pm_find_src(me, &src);
        else memset(&src, 0, sizeof src);
        if (pm_tx_step(&PMT, tag, &l0, &l1, &src, now, &m)) {
            uint8_t buf[PM_LEN];
            pm_enc(buf, &m);
            if (w1_send_ext(&N.w, CP_W1_PMOT, buf, PM_LEN)) PMS.tx++;
            else PMS.tx_fail++;
            if (m.kind != PM_KEEP && PMS.lines++ < 300)
                wlog("pmot: this PC's human %s %s (motion %04x%s): told the partner (event %u)", m.kind == PM_BEGIN ? "begins" : m.kind == PM_END ? "ends" : "goes on in", m.tag < PM_T_N ? pm_states[m.tag].name : "?",
                     m.l0.id, m.has_src ? ", bank 0xE of an object" : "", m.ev);
        }
    }
    /* receiver */
    uint32_t pu = 0;
    wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &pu);
    if (pu != pm_unit) {   /* another puppet unit (a section, a restart): nothing of the old one is touched */
        if (pm_rx_drop(&PMR)) PMS.drops++;
        pm_unit = pu, pm_par_bound = 0;
    }
    if (!f_pmot || !pu) return;
    int pidx = pm_puppet_idx(pu), hold = pm_hold_now(pu);
    unsigned busy0 = PMR.refused_busy, adopt0 = PMR.adopted;
    int frozen = pidx == PM_IDX_EVENTWAIT && (rd32(pu + 0xF44u) & 8u) != 0;
    int a = pm_rx_step_f(&PMR, pidx, hold, frozen, now);
    if (PMR.adopted != adopt0 && PMS.lines++ < 300)
        wlog("pmot: puppet %08x already waits in an EventWaitState of this PC's own code (not ours, no hold here, the partner acts on his PC): the mirror takes it for the partner's %s (event %u)",
             (unsigned)pu, PMR.cur.tag < PM_T_N ? pm_states[PMR.cur.tag].name : "?", (unsigned)PMR.ev);
    if (PMR.refused_busy != busy0 && pm_busy_ev != PMR.ev && PMS.lines++ < 300) {   /* once per event of the partner */
        char nb[48];
        pm_busy_ev = PMR.ev;
        wlog("pmot: puppet %08x is busy in his lower state %s: the partner's %s (event %u) is not mirrored while it lasts", (unsigned)pu, pm_state_name(pu, nb, sizeof nb),
             PMR.cur.tag < PM_T_N ? pm_states[PMR.cur.tag].name : "?", (unsigned)PMR.ev);
    }
    if (a & PM_A_DROP) {
        PMS.drops++;
        if (PMS.lines++ < 300) wlog("pmot: puppet %08x: the mirror lets go (%s)", (unsigned)pu, hold ? "a cutscene / load / death / downed holds him" : "his state machine took him");
    }
    if (a & PM_A_REQ_FREEZE) pm_request(pu, PM_IDX_EVENTWAIT, "the partner interacts");
    if (a & PM_A_UNBIND) pm_bind(pu, 0);
    if (a & PM_A_BIND) pm_bind(pu, 1);
    if (a & PM_A_MOTION) pm_motion(pu);
    if (a & PM_A_UNFREEZE) {   /* the stock release of the stock freeze (logic thread, the unit proven above) */
        int done = 0;
        if (pm_unf_ok > 0 && (rd32(pu + 0xF44u) & 8u)) {
            ops_enter();
            done = (((unsigned(TC *)(void *))(uintptr_t)PM_FN_UNFREEZE)((void *)(uintptr_t)pu) & 0xFF) != 0;
            ops_leave();
            (void)done;
            PMS.unfreezes++;
        }
        if (PMS.lines++ < 300)
            wlog("pmot: puppet %08x: the stock freeze of this PC (FUN_006B3B80, +0xF44 bit 3) %s -> FUN_006AE390 %s", (unsigned)pu,
                 a & PM_A_REQ_RELEASE ? "ends with the partner's interaction" : "holds him with no hold here and no event of the partner (he is free on his PC)",
                 pm_unf_ok > 0 ? "called" : "NOT verified: not called");
    }
    if (a & PM_A_REQ_RELEASE) pm_request(pu, PM_IDX_MOVE, "the partner's interaction ended");
}

static void pmot_reset(void)
{
    pm_tx_reset(&PMT);
    if (pm_rx_drop(&PMR)) PMS.drops++;
    pm_unit = 0, pm_par_bound = 0, pm_busy_ev = 0x100;
}

static void pmot_status(char *out, int size)
{
    uint32_t pu = 0, me = 0;
    char np[48] = "-", nm[48] = "-";
    if (call_ok(WH_C_CURSTATE)) {
        if (wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &pu) && pu) pm_state_name(pu, np, sizeof np);
        if (wh_player_by_slot(&MEM, (unsigned)g_own, &me) && me) pm_state_name(me, nm, sizeof nm);
    }
    int o = snprintf(out, (size_t)size, "the partner's puppet %08x: lower state %s | this PC's human %08x: %s\n", (unsigned)pu, np, (unsigned)me, nm);
    if (o < 0 || o >= size) return;
    out += o, size -= o;
    snprintf(out, (size_t)size,
             "world pmot (pmot1.h): pmot %d, pmtrav %d | DTIs %s, binders %s | sender: begins %u, changes %u, keeps %u, ends %u, sent %u (not queued %u) | receiver: words %u (bad %u, old %u), events %u, "
             "state %d, holds %u (timeouts %u), motions %u (refused %u), binds %u (refused %u, params %u), releases %u, silent %u, too long %u, busy %u, adopted %u, stock unfreezes %u, dropped by holds %u, taken by his state machine %u, "
             "no object here %u, requests %u (refused %u), drops %u",
             f_pmot, f_pmtrav, pm_ids_done ? (pm_id_evwait && pm_id_move ? "verified" : "NOT verified") : "not checked", pm_fn_ok < 0 ? "not checked" : pm_fn_ok ? "verified" : "REFUSED", PMT.begins, PMT.mots,
             PMT.keeps, PMT.ends, PMS.tx, PMS.tx_fail, PMS.rx, PMS.bad, PMR.old, PMR.events, PMR.st, PMR.freezes, PMR.freeze_timeouts, PMS.motions, PMS.mot_refused, PMS.binds, PMS.bind_refused, PMS.params,
             PMR.releases, PMR.silent, PMR.too_long, PMR.refused_busy, PMR.adopted, PMS.unfreezes, PMR.held_drops, PMR.foreign_exit, PMR.no_src, PMS.reqs, PMS.req_refused, PMS.drops);
}
