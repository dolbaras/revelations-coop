/*
 * lid1_game.inc - included by world1_game.c after pmot1_game.inc: sync022 rows W2 / W3 (lid1.h), the partner's locker door / weapon box lid opens on this PC
 * as a picture. Logic thread only (the world frame between the unit updates, and op_ext of the same frame):
 *   sender: every 100 ms the boxes of the object list (class uLockerDoor 80061200, uTerragrigiaLockerDoor 8006120B, uOMCorpseBox 80061638, uOMSafeDoor
 *   80061624, uWeaponBox 80061205) by their state DTI (the manager at unit+0xE80, 0x49C3E0) and their layer-0 motion id (unit+0x2D0+4) -> ext CP_W1_LID;
 *   receiver: the copy by key4, its class, its vtable's slot 26 = 0x4BBA80 (setMotion on layer 0) and slot 27 = 0x6CD200 (the objects' layer setter), its state
 *   = Closed -> slot 26 (id, 0, 0, 1.0, 4) as the stock enter does it. No state, no item, no GUI, no flag.
 * `world set lids 0|1` (default 1), `world lids` = status (the boxes near this PC's human: where to stand in the lab).
 */
#include "lid1.h"

#define LD_FN_SETMOT26 0x4BBA80u   /* the objects' vtable slot 26: thiscall(obj; id, blend, frame, speed, flags) ret 0x14 -> slot 27 with layer 0 */
#define LD_FN_SETMOT27 0x6CD200u   /* the objects' slot 27 (the layer setter behind the net gate FUN_0049F560: 1 offline) */

static volatile int f_lids = 1;
static ld_tx LDT;
static ld_rx LDR;
static uint32_t ld_last_ms;
static int ld_ids_done, ld_fn_ok = -1;
/* the state DTIs (dti-rev1.txt): uOpenBox Closed / Opening / Opened / Closing / ItemSync, uWeaponBox Closed / Opening / Opened / Closing */
enum { LDD_BOX_CLOSED, LDD_BOX_OPENING, LDD_BOX_OPENED, LDD_BOX_CLOSING, LDD_BOX_ITEMSYNC, LDD_WB_CLOSED, LDD_WB_OPENING, LDD_WB_OPENED, LDD_WB_CLOSING, LDD_N };
static const struct { uint32_t dti; const char *name; int tag; } ld_dti[LDD_N] = {
    {0x011797D4u, "ClosedState", LD_S_CLOSED},  {0x01179814u, "OpeningState", LD_S_OPENING}, {0x011797F4u, "OpenedState", LD_S_OPENED},
    {0x01179794u, "ClosingState", LD_S_CLOSING}, {0x01179774u, "ItemSyncState", LD_S_OTHER},  {0x0117A1ACu, "ClosedState", LD_S_CLOSED},
    {0x0117A1CCu, "OpeningState", LD_S_OPENING}, {0x0117A18Cu, "OpenedState", LD_S_OPENED},  {0x0117A16Cu, "ClosingState", LD_S_CLOSING},
};
static uint32_t ld_ids[LDD_N];
static struct { unsigned passes, boxes, sent, send_fail, calls, lines; } LDS;
typedef void (TC *ld_setmot_fn)(void *obj, int id, float blend, float frame, float speed, int flags);

static int ld_class_kind(uint32_t cls)   /* 1 = uOpenBox family, 2 = uWeaponBox, 0 = not a box */
{
    return cls == 0x80061200u || cls == 0x8006120Bu || cls == 0x80061638u || cls == 0x80061624u ? 1 : cls == 0x80061205u ? 2 : 0;
}
static void ld_resolve(void)
{
    if (ld_ids_done) return;
    int all = 1;
    for (int i = 0; i < LDD_N; i++) {
        uint32_t vt = rd32(ld_dti[i].dti), nm = rd32(ld_dti[i].dti + 4), id = rd32(ld_dti[i].dti + 0x1C);
        char s[24] = "";
        if (nm) MEM.read(MEM.user, nm, s, sizeof s - 1);
        ld_ids[i] = vt >= WH_DTI_VT_LO && vt < WH_DTI_VT_HI && id && !strcmp(s, ld_dti[i].name) ? id : 0;
        if (!ld_ids[i]) all = 0;
    }
    /* slot 26's body: mov edx,[esp+0x14]; movss xmm0,[esp+0x10]; mov eax,[ecx]; mov eax,[eax+0x6c] (-> slot 27, layer 0) */
    static const uint8_t b26[] = {0x8B, 0x54, 0x24, 0x14, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x10, 0x8B, 0x01, 0x8B, 0x40, 0x6C};
    uint8_t c[sizeof b26];
    ld_fn_ok = all && MEM.read(MEM.user, LD_FN_SETMOT26, c, sizeof c) && !memcmp(c, b26, sizeof b26);
    ld_ids_done = 1;
    wlog("lids: box state DTIs %s, the objects' motion setter (slot 26 0x4BBA80) %s", all ? "verified" : "NOT all verified (the mirror is off)", ld_fn_ok ? "verified" : "REFUSED");
}
/* the state tag of a box: its kind's DTI ids; LD_S_UNKNOWN when unreadable */
static int ld_tag(uint32_t unit, int kind)
{
    uint32_t id = cur_state(unit);
    if (!id) return LD_S_UNKNOWN;
    int lo = kind == 1 ? LDD_BOX_CLOSED : LDD_WB_CLOSED, hi = kind == 1 ? LDD_BOX_ITEMSYNC : LDD_WB_CLOSING;
    for (int i = lo; i <= hi; i++)
        if (ld_ids[i] && ld_ids[i] == id) return ld_dti[i].tag;
    return LD_S_OTHER;   /* a surprise (an enemy in the locker), a wait ... */
}
static uint16_t ld_motion(uint32_t unit) { return (uint16_t)rd16(unit + MO_LAYER_OFF + 4); }
static int ld_setter_ok(uint32_t unit)
{
    uint32_t vt = rd32(unit);
    return vt && rd32(vt + 0x68) == LD_FN_SETMOT26 && rd32(vt + 0x6C) == LD_FN_SETMOT27;
}

static void lid_pass(uint32_t now)
{
    if (!f_lids || (int32_t)(now - ld_last_ms) < 100) return;
    ld_last_ms = now;
    ld_resolve();
    uint32_t sa = sarrange();
    if (!sa || !call_ok(WH_C_CURSTATE) || ld_fn_ok <= 0) return;
    LDS.passes++;
    ld_tx_pass(&LDT);
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_OBJECTS), boxes = 0;
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_OBJECTS, i, &e) || !e.unit) continue;
        uint32_t cls = wh_unit_class(&MEM, e.unit);
        int kind = ld_class_kind(cls);
        if (!kind) continue;
        int tag = ld_tag(e.unit, kind);
        if (tag == LD_S_UNKNOWN) continue;
        boxes++;
        ld_msg m;
        if (ld_tx_step(&LDT, e.k, cls, tag, ld_motion(e.unit), &m)) {
            uint8_t buf[LD_LEN];
            ld_enc(buf, &m);
            int ok = w1_send_ext(&N.w, CP_W1_LID, buf, LD_LEN);
            if (ok) LDS.sent++;
            else LDS.send_fail++;
            if (LDS.lines++ < 200)
                wlog("lids: box %08x class %08x key %08x/%08x/%u/%u: this PC's own human %s it (motion %u): %s to the partner", (unsigned)e.unit, (unsigned)cls, (unsigned)e.k[0], (unsigned)e.k[1],
                     (unsigned)e.k[2], (unsigned)e.k[3], m.kind == LD_OPEN ? "opens" : "closes", m.id, ok ? "told" : "NOT told");
        }
    }
    LDS.boxes = boxes;
}

/* a word of the partner (op_ext, the logic thread of the same frame) */
static void lid_rx(const uint8_t *b, unsigned len)
{
    ld_msg m;
    LDR.rx++;
    if (!ld_dec(b, len, &m)) {
        LDR.bad++;
        return;
    }
    if (!f_lids) return;
    ld_resolve();
    uint32_t sa = sarrange();
    wh_entry e;
    int have = sa && ld_fn_ok > 0 && call_ok(WH_C_CURSTATE) && wh_find_key4(&MEM, sa, WH_LIST_OBJECTS, m.k4, &e) && e.unit;
    uint32_t cls = have ? wh_unit_class(&MEM, e.unit) : 0;
    int kind = ld_class_kind(cls), same = have && cls == m.cls && kind && ld_setter_ok(e.unit);
    int tag = same ? ld_tag(e.unit, kind) : LD_S_UNKNOWN;
    uint16_t id = same ? ld_motion(e.unit) : 0xFFFF;
    int d = ld_rx_decide(&LDR, &m, have, same, tag, id);
    if (d == LD_PLAY) {
        uint32_t fn = rd32(rd32(e.unit) + 0x68);
        ops_enter();
        ((ld_setmot_fn)(uintptr_t)fn)((void *)(uintptr_t)e.unit, m.id, 0.0f, 0.0f, 1.0f, 4);
        ops_leave();
        LDS.calls++;
    }
    if (LDS.lines++ < 200)
        wlog("lids: the partner %s box %08x/%08x/%u/%u (class %08x): %s", m.kind == LD_OPEN ? "opened" : "closed", (unsigned)m.k4[0], (unsigned)m.k4[1], (unsigned)m.k4[2], (unsigned)m.k4[3],
             (unsigned)m.cls,
             d == LD_PLAY ? (m.kind == LD_OPEN ? "its copy here plays the opening (motion 1) and stays closed for this PC's player" : "its copy here plays the closing (motion 3)")
             : d == LD_R_NOCOPY ? "no copy here (another area): nothing to do"
             : d == LD_R_CLASS ? "the copy here is another class / setter: left alone"
             : d == LD_R_BUSY ? "the copy here is not closed (this PC's own player uses or opened it): left alone"
             : d == LD_R_NOTSHOWN ? "the copy here does not show the opening: left alone" : "not the open / close motion: left alone");
}

static void lid_reset(void) { ld_tx_reset(&LDT); }

static void lid_status(char *out, int size)
{
    int o = snprintf(out, (size_t)size,
                     "world lids (lid1.h): lids %d, state DTIs / setter %s | passes %u, boxes now %u | told: opens %u, closes %u (not queued %u), first sights %u | received %u (bad %u): played open %u, "
                     "close %u | left alone: no copy %u, other class %u, not closed here %u, not shown %u, other motion %u | setter calls %u",
                     f_lids, ld_fn_ok < 0 ? "not checked" : ld_fn_ok ? "verified" : "REFUSED", LDS.passes, LDS.boxes, LDT.opens, LDT.closes, LDS.send_fail, LDT.first_seen, LDR.rx, LDR.bad, LDR.played_open,
                     LDR.played_close, LDR.r_nocopy, LDR.r_class, LDR.r_busy, LDR.r_notshown, LDR.r_id, LDS.calls);
    ld_resolve();
    uint32_t sa = sarrange(), me = 0;
    wh_player_by_slot(&MEM, (unsigned)g_own, &me);
    unsigned n = sa ? wh_list_count(&MEM, sa, WH_LIST_OBJECTS) : 0, shown = 0;
    static const char *const tn[] = {"?", "Closed", "Opening", "Opened", "Closing", "other"};
    for (unsigned i = 0; i < n && o > 0 && o < size - 220 && shown < 16; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_OBJECTS, i, &e) || !e.unit) continue;
        uint32_t cls = wh_unit_class(&MEM, e.unit);
        int kind = ld_class_kind(cls);
        if (!kind) continue;
        int tag = ld_tag(e.unit, kind);
        float dx = me ? rdf(e.unit + WH_U_POS) - rdf(me + WH_U_POS) : 0, dz = me ? rdf(e.unit + WH_U_POS + 8) - rdf(me + WH_U_POS + 8) : 0;
        o += snprintf(out + o, (size_t)(size - o), "\n  box %08x class %08x key %08x/%08x/%u/%u pos (%.0f,%.0f,%.0f) dist %.0f state %s motion %u", (unsigned)e.unit, (unsigned)cls, (unsigned)e.k[0],
                      (unsigned)e.k[1], (unsigned)e.k[2], (unsigned)e.k[3], (double)rdf(e.unit + WH_U_POS), (double)rdf(e.unit + WH_U_POS + 4), (double)rdf(e.unit + WH_U_POS + 8),
                      (double)sqrtf(dx * dx + dz * dz), tn[tag >= 0 && tag <= 5 ? tag : 0], ld_motion(e.unit));
        shown++;
    }
}
