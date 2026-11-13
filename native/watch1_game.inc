/*
 * watch1_game.inc - included by world1_game.c right after local1_game.inc: rev1 live round 13, task 3 (watch1.h is the rule and the evidence).
 *   - client (logic thread, model lock): watch_tx = the records of the enemies run on this PC's AI (local1.h "local") every WT_TX_MS (ext CP_W1_RLOC);
 *   - host: watch_rx stores them and applies them at once (watch_apply_one: kind / owner through lr_set_kind, position with the move object's teleport byte when
 *     far, hp); watch_pass re-checks every key every 250 ms (the take-back when the records stop or the host's active human comes near);
 *   - both: the list of replicas near the watched human (rule 4) for the zone-pass hook (engine thread: watch_after_zpass clears their hidden bit right after the
 *     pass, after checking that the list entry still points at the unit);
 *   - both: the HUD minimap of the watching PC (rule 5): a private copy of the stock uMiniMapManager's vtable whose update slots 8 / 9 run with the own slot
 *     answering the watched human's slot (capture_own_scope, split.c's way for its second set); the copy stays on the object (it passes straight through while
 *     this PC does not watch) and is taken off when the hooks are reverted.
 * Switches: `world set watchrep 0` (no reverse replicas, nothing un-hidden), `world set rloc 0` (the client sends nothing), `world set watchmap 0`; `world watch`.
 */
#ifndef FC
#define FC __attribute__((fastcall))
#endif

static struct {
    unsigned tx_rounds, tx_msgs, tx_recs, tx_fail, applied, snaps, kind_made, kind_back, kind_fail, rearr, vis_lists, vis_units, vis_cleared, vis_stale, map_installs,
        map_scoped, map_reverts, lines, host_blows;   /* host_blows: fix021 rule 6 */
} WS;
static uint32_t wt_last_pass_ms;
#define WS_LOG(...) do { if (++WS.lines <= 200) wlog(__VA_ARGS__); } while (0)

/* ---- client: rule 1 */
static void watch_tx(uint32_t now)
{
    if (!g_client || !f_rloc || !wt_tx_due(&WTX, now)) return;
    wt_rec r[WT_MAX];
    unsigned n = 0;
    for (unsigned i = 0; i < LR_MAX && n < WT_MAX; i++) {
        lr_ent *e = &LR.e[i];
        if (!e->used || !e->unit) continue;
        uint32_t u = e->unit;
        float hp = rdf(u + WH_U_HP);
        uint32_t st = rd32(u + 0xC) & 7, e1c = rd32(u + WH_U_FLAGS);
        int alive = hp > 0.0f && (st == 1 || st == 2) && !(e1c & 0x80000000u) && rd32(u + WH_U_KIND) == 2;
        if (!wt_tx_wants(e->local, e->reported, alive)) continue;
        r[n].key = e->key, r[n].hp = hp;
        r[n].pos[0] = rdf(u + WH_U_POS), r[n].pos[1] = rdf(u + WH_U_POS + 4), r[n].pos[2] = rdf(u + WH_U_POS + 8);
        n++;
    }
    if (!n) return;
    WS.tx_rounds++;
    for (unsigned k = 0; k < n; k += WT_PER_MSG) {
        uint8_t b[WT_MSG_MAX];
        unsigned m = n - k < WT_PER_MSG ? n - k : WT_PER_MSG;
        unsigned len = wt_enc(b, ++WTX.seq, r + k, m);
        if (w1_send_ext(&N.w, CP_W1_RLOC, b, len)) WS.tx_msgs++, WS.tx_recs += m, WTX.msgs++, WTX.recs += m;
        else WS.tx_fail++;
    }
    if (WS.tx_rounds == 1 || WS.tx_rounds % 400 == 0)
        WS_LOG("watch: this PC runs %u enemies on its own AI: their records go to the host (round %u, first %u/%u at (%.0f,%.0f,%.0f) hp %.0f)", n, WS.tx_rounds, r[0].key >> 8, r[0].key & 0xFF,
               r[0].pos[0], r[0].pos[1], r[0].pos[2], r[0].hp);
}

/* ---- host: rule 2 / 3 for one key */
static void watch_apply_one(wt_ent *e, uint32_t now)
{
    uint32_t sa = sarrange();
    wh_entry we;
    unsigned b = e->key >> 8, ix = e->key & 0xFF;
    if (!sa || !wh_find_key(&MEM, sa, WH_LIST_ENEMIES, b, ix, &we)) return;
    uint32_t own_u = 0;
    wh_player_by_slot(&MEM, (unsigned)g_own, &own_u);
    int own_active = !g_solo_watch;
    uint32_t vu = own_active ? own_u : g_solo_watched;   /* the host streams the stage around its view point (stage1.h) */
    float vx = vu ? rdf(vu + WH_U_POS) - e->pos[0] : 1e9f, vz = vu ? rdf(vu + WH_U_POS + 8) - e->pos[2] : 0.0f;
    float view_dist = sqrtf(vx * vx + vz * vz);
    if (!we.unit && f_watchrep && wt_fresh(e, now) && view_dist <= WT_NEAR && !rd8(we.entry + WH_ENTRY_SPENT)) {   /* the client runs it near the host's view, no unit here: the rearrange makes one */
        if (local_rearrange_record(sa, b, ix)) WS.rearr++;
        if (!wh_find_key(&MEM, sa, WH_LIST_ENEMIES, b, ix, &we)) return;
    }
    uint32_t u = we.unit;
    float hp = u ? rdf(u + WH_U_HP) : 0.0f;
    uint32_t st = u ? rd32(u + 0xC) & 7 : 0;
    int alive = u && hp > 0.0f && (st == 1 || st == 2) && !(rd32(u + WH_U_FLAGS) & 0x80000000u);
    unsigned kind = alive ? rd32(u + WH_U_KIND) : 0;
    float dx = own_u ? rdf(own_u + WH_U_POS) - e->pos[0] : 1e9f, dz = own_u ? rdf(own_u + WH_U_POS + 8) - e->pos[2] : 0.0f;
    float own_dist = sqrtf(dx * dx + dz * dz);
    int hidden = u && (rd32(u + WH_U_FLAGS) & LR_E1C_HIDDEN) != 0;
    int a = !f_watchrep ? (e->active ? wt_host_step(&WTH, e, kind, 1, 0.0f, 0.0f, now) : WT_KEEP)
            : f_handover ? wt_host_step_zone(&WTH, e, kind, own_active, own_dist, view_dist, hidden, now)   /* fix021: by the host's zone (rules 2z / 3z) */
                         : wt_host_step(&WTH, e, kind, own_active, own_dist, view_dist, now);
    if (a == WT_MAKE_REPLICA) {
        int ok = lr_set_kind(we.entry, u, 3);
        if (ok) WS.kind_made++;
        else WS.kind_fail++;
        WS_LOG("watch: enemy %u/%u: unit %08x (kind %u, E1C %08x) runs on the CLIENT's AI: a replica here now (kind 3, the client's slot as owner)%s; this PC %s, its human %.0f away",
               b, ix, (unsigned)u, kind, rd32(u + WH_U_FLAGS), ok ? "" : ": the kind setter REFUSED", own_active ? "plays" : "WATCHES", own_dist);
        kind = ok ? 3 : kind;
    } else if (a == WT_TAKE_BACK) {
        int ok = lr_set_kind(we.entry, u, 2);
        if (ok) WS.kind_back++;
        else WS.kind_fail++;
        WS_LOG("watch: enemy %u/%u: unit %08x back on THIS PC's AI (kind 2%s): %s; it continues from the client's state at (%.0f,%.0f,%.0f) hp %.0f (E1C %08x)", b, ix, (unsigned)u,
               ok ? "" : " REFUSED", !wt_fresh(e, now) ? "the client's records stopped" : f_handover && own_active ? "awake in this PC's human's zone" : "this PC's active human is near",
               rdf(u + WH_U_POS), rdf(u + WH_U_POS + 4), rdf(u + WH_U_POS + 8), rdf(u + WH_U_HP), rd32(u + WH_U_FLAGS));
        return;
    }
    if (!e->active || !alive || kind != 3 || !wt_fresh(e, now)) return;
    float cur[3] = {rdf(u + WH_U_POS), rdf(u + WH_U_POS + 4), rdf(u + WH_U_POS + 8)};
    uint32_t mo = rd32(u + 0xE88);
    if (mo && wh_snap_needed(cur, e->pos, (float)(f_snap ? f_snap : 300))) {   /* far (a new replica at its spawn point, another floor): the teleport byte, as op_update */
        uint32_t zero = 0;
        wr8(mo + 0x2B4, 1);
        wr(u + WH_U_POS, e->pos, 12);
        wr(u + 0x4C, &zero, 4);
        WS.snaps++;
    } else {
        wr(u + WH_U_POS, e->pos, 12);
    }
    float mh = f_handover ? wt_mirror_hp(e->hp, e->hd) : e->hp;   /* fix021 rule 6: the host's own blows stay taken off */
    if (e->hp > 0.0f && fabsf(mh - hp) > 0.01f) wrf(u + WH_U_HP, mh);
    WS.applied++;
}

/* host: a message of the client (op_ext, logic thread, model lock) */
static void watch_rx(const uint8_t *b, unsigned len)
{
    if (bound_role != W1_HOST) return;
    uint32_t now = cs_now(bound_cs);
    unsigned old = WTH.rx_recs;
    if (!wt_rx(&WTH, b, len, now) || WTH.rx_recs == old) return;
    if (WTH.rx_msgs == 1) WS_LOG("watch: the client's first records of the enemies it runs on its own AI arrived");
    for (unsigned i = 0; i < WT_MAX; i++)
        if (WTH.e[i].used && WTH.e[i].rx_ms == (now ? now : 1)) watch_apply_one(&WTH.e[i], now);
}

/* ---- rule 4 (watch1.h) + rule 7 (zkeep1.h, rev1 live 17): the list for the zone-pass hook (logic thread). Rule 4: the watching PC keeps the replicas near
 * the watched human visible. Rule 7: the client keeps the replicas the host still simulates near its own active human visible and hittable (m16a1: the
 * guest's human crossed a zone border mid-fight and its engine slept the host's Ooze: every shot went through). Filled into the buffer the engine thread does
 * not read, then switched. */
static const ew_ent *zk_ew_of(uint32_t entry)
{
    for (unsigned i = 0; i < EW_MAX; i++)
        if (EW[i].used && EW[i].entry == entry && EW[i].has_key) return &EW[i];
    return NULL;
}
static void watch_visible_list(uint32_t now)
{
    uint32_t sa = sarrange(), wu = g_solo_watch ? g_solo_watched : 0, own_u = 0;
    LONG nb = 1 - (wv_cur & 1);
    unsigned k = 0, kept = 0;
    int r4 = f_watchrep && g_solo_watch && wu, r7 = f_zkeep && g_client && !g_solo_watch;
    if (sa && r7) wh_player_by_slot(&MEM, (unsigned)g_own, &own_u);
    if (sa && (r4 || (r7 && own_u))) {
        float wx = r4 ? rdf(wu + WH_U_POS) : 0.0f, wz = r4 ? rdf(wu + WH_U_POS + 8) : 0.0f;
        float ox = own_u ? rdf(own_u + WH_U_POS) : 0.0f, oz = own_u ? rdf(own_u + WH_U_POS + 8) : 0.0f;
        unsigned n = wh_list_count(&MEM, sa, WH_LIST_ENEMIES);
        for (unsigned i = 0; i < n && k < WV_MAX; i++) {
            wh_entry e;
            if (!wh_list_at(&MEM, sa, WH_LIST_ENEMIES, i, &e) || !e.unit) continue;
            uint32_t u = e.unit, kind = rd32(u + WH_U_KIND);
            if (kind != 3) continue;
            float hp = rdf(u + WH_U_HP);
            uint32_t st = rd32(u + 0xC) & 7;
            int alive = hp > 0.0f && (st == 1 || st == 2) && !(rd32(u + WH_U_FLAGS) & 0x80000000u) && enemy_unit(u);
            int keep = 0;
            if (r4) {
                float dx = rdf(u + WH_U_POS) - wx, dz = rdf(u + WH_U_POS + 8) - wz;
                if (!wt_keep_visible(f_watchrep, g_solo_watch, kind, alive, sqrtf(dx * dx + dz * dz))) continue;
            } else {
                const ew_ent *w = zk_ew_of(e.entry);
                const lr_ent *le = w ? lr_find(&LR, w->key, 0) : NULL;
                float dx = rdf(u + WH_U_POS) - ox, dz = rdf(u + WH_U_POS + 8) - oz, d = sqrtf(dx * dx + dz * dz);
                if (!le || !zk_keep(f_zkeep, g_client, g_solo_watch, kind, alive, lr_host_simulates(le, now), le->reported, d)) continue;
                keep = 1, kept++;
                int seen = 0;
                for (unsigned j = 0; j < zk_nsaid; j++) seen |= zk_said[j] == u;
                if (!seen && zk_nsaid < 16) {
                    zk_said[zk_nsaid++] = u;
                    wlog("zkeep: replica %u/%u unit %08x: the host simulates it, this PC's human %.0f away: kept visible and hittable across this PC's own zone passes (E1C %08x)",
                         w->key >> 8, w->key & 0xFF, (unsigned)u, (double)d, rd32(u + WH_U_FLAGS));
                }
            }
            wv_unit[nb][k] = u, wv_entry[nb][k] = e.entry, wv_keep[nb][k] = (uint8_t)keep, k++;
        }
    }
    InterlockedExchange(&wv_cnt[nb], (LONG)k);
    InterlockedExchange(&wv_cur, nb);
    if (r4) WS.vis_lists++, WS.vis_units += k;
    if (r7) InterlockedIncrement(&ZK.lists), InterlockedExchangeAdd(&ZK.listed_units, (LONG)kept);
    if (r4) {
        static unsigned said_k = 0xFFFFFFFFu, said_enters;
        if ((k != said_k || said_enters != g_solo_enters) && WS.lines < 200) {
            said_k = k, said_enters = g_solo_enters;
            WS_LOG("watch: this PC WATCHES: %u replica(s) within %.0f of the watched human are kept visible after the zone pass", k, (double)WT_NEAR);
        }
    }
}

/* engine thread, right after FUN_00460250 (hook_zpass): the hidden bit of the listed replicas goes again (the pass sets it in every frame) */
static void watch_after_zpass(void)
{
    LONG c = wv_cur & 1, n = wv_cnt[c];
    if (!g_active || n <= 0) return;
    for (LONG i = 0; i < n && i < WV_MAX; i++) {
        uint32_t u = wv_unit[c][i], en = wv_entry[c][i];
        if (!u || !en || rd32(en + 4) != u) {   /* the entry does not point at the unit any more (a rearrange / a removal since the list was made) */
            InterlockedIncrement((volatile LONG *)&WS.vis_stale);
            continue;
        }
        uint32_t f = rd32(u + WH_U_FLAGS);
        if (f & LR_E1C_HIDDEN) {
            wr32(u + WH_U_FLAGS, f & ~LR_E1C_HIDDEN);
            InterlockedIncrement((volatile LONG *)&WS.vis_cleared);
            if (wv_keep[c][i]) InterlockedIncrement(&ZK.cleared);
        }
    }
}

/* ---- rule 5: the stock HUD minimap of the watching PC follows the watched human */
#define WM_SLOTS 20
static uint32_t *wm_vt;                 /* the private copy (never freed: an object made from it may outlive an install) */
static uint32_t wm_stock_vt, wm_obj;
static void (FC *wm_orig8)(unsigned), (FC *wm_orig9)(unsigned);
static void FC wm_wrap8(unsigned self)
{
    int s = wt_map_scope(f_watchmap, g_solo_watch, g_own);
    if (s >= 0) capture_own_scope(s), InterlockedIncrement((volatile LONG *)&WS.map_scoped);
    wm_orig8(self);
    if (s >= 0) capture_own_scope(-1);
}
static void FC wm_wrap9(unsigned self)
{
    int s = wt_map_scope(f_watchmap, g_solo_watch, g_own);
    if (s >= 0) capture_own_scope(s);
    wm_orig9(self);
    if (s >= 0) capture_own_scope(-1);
}
static uint32_t watch_map_obj(void)
{
    uint32_t cockpit = rd32(0x1169120u);   /* sIDCockpit; +0x2C = the stock uMiniMapManager (minimap1 notes, split.c) */
    return cockpit ? rd32(cockpit + 0x2C) : 0;
}
static void watch_map_pass(void)
{
    if (!f_watchmap || !g_solo_watch) return;   /* installed only when a watch begins; it stays (pass-through) until the hooks are reverted */
    uint32_t mm = watch_map_obj(), vt = mm ? rd32(mm) : 0;
    if (!mm || !vt || (wm_vt && vt == (uint32_t)(uintptr_t)wm_vt)) return;
    char cn[64] = "";
    if (!classes_object_name(mm, cn, sizeof cn) || strcmp(cn, "uMiniMapManager")) return;
    if (!wm_vt) {
        uint32_t *c = VirtualAlloc(NULL, WM_SLOTS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!c) return;
        for (int i = 0; i < WM_SLOTS; i++) c[i] = rd32(vt + 4u * (unsigned)i);
        wm_orig8 = (void (FC *)(unsigned))(uintptr_t)c[8], wm_orig9 = (void (FC *)(unsigned))(uintptr_t)c[9];
        c[8] = (uint32_t)(uintptr_t)wm_wrap8, c[9] = (uint32_t)(uintptr_t)wm_wrap9;
        wm_vt = c, wm_stock_vt = vt;
    } else if (vt != wm_stock_vt) {
        return;   /* another vtable than the one the copy was made of: left alone */
    }
    wr32(mm, (uint32_t)(uintptr_t)wm_vt);
    wm_obj = mm, WS.map_installs++;
    WS_LOG("watch: the HUD minimap (uMiniMapManager %08x) follows the watched human while this PC watches (own slot %d answers %d in its update)", (unsigned)mm, (int)g_own, 1 - (int)g_own);
}
static void watch_map_revert(void)
{
    uint32_t mm = watch_map_obj();
    if (wm_vt && mm && rd32(mm) == (uint32_t)(uintptr_t)wm_vt) wr32(mm, wm_stock_vt), WS.map_reverts++;
    wm_obj = 0;
}

/* ---- rev1 live 14 (watchcam1.h, task 2): the watching PC's camera = the watched human's own view */
#include "watchcam1.h"
static volatile int f_watchcam = 1;     /* both: send the own gameplay camera while the other human is switched off / show the received one while watching */
static wc_rx_t WCR;                     /* logic thread */
static uint16_t wc_tx_seq;
static uint32_t wc_tx_ms;
static struct { unsigned tx, tx_fail, installs, reverts, lines; volatile LONG writes, stale; } WCS;
static volatile LONG wc_pub_seq;        /* seqlock of the engine thread's copy: odd while the logic thread writes it */
static wc_cam wc_pub;
static volatile uint32_t wc_pub_ms;
#define WC_SLOTS 21                     /* uCameraManage's vtable 0xFF6C00: slots 0..20 (the strings follow) */
static uint32_t *wc_vt;
static uint32_t wc_stock_vt;
static volatile uint32_t wc_obj;
static void (FC *wc_orig9)(unsigned);
static wc_cam wc_cur;                   /* engine thread */
static int wc_have_cur;
static void FC wc_wrap9(unsigned self)
{
    wc_orig9(self);   /* the stock update: the mode's camera into +0x3C / +0x40 / +0x50 / +0x60 */
    if (!f_watchcam || !g_solo_watch || self != wc_obj) {
        wc_have_cur = 0;
        return;
    }
    wc_cam t;
    LONG s0 = 0, s1 = 1;
    for (int k = 0; k < 4 && s0 != s1; k++) {
        s0 = wc_pub_seq;
        if (s0 & 1) continue;
        t = wc_pub;
        s1 = wc_pub_seq;
    }
    if (s0 != s1 || !s0 || (uint32_t)(GetTickCount() - wc_pub_ms) > WC_STALE_MS) {   /* nothing fresh from the active PC: the stock manager 1 camera */
        wc_have_cur = 0;
        InterlockedIncrement(&WCS.stale);
        return;
    }
    wc_smooth(&wc_cur, &wc_have_cur, &t, WC_ALPHA);
    volatile float *f = (volatile float *)(uintptr_t)self;
    f[0x3C / 4] = wc_cur.fov;
    for (int i = 0; i < 3; i++) f[0x40 / 4 + i] = wc_cur.eye[i], f[0x50 / 4 + i] = wc_cur.up[i], f[0x60 / 4 + i] = wc_cur.tgt[i];
    InterlockedIncrement(&WCS.writes);
}
/* the active PC: its gameplay camera while the other human is switched off (watch_pass, every frame) */
static void watchcam_tx(uint32_t now)
{
    int own = g_own;
    if (own != 0 && own != 1) return;
    uint32_t root = rd32(SOLO_CAMROOT), m0 = root ? rd32(root + 0xCE0) : 0, vp0 = root ? rd32(root + 0x30 + 4) : 0;
    if (!wc_tx_due(f_watchcam, solo_u[own].off, solo_u[1 - own].off, m0 && vp0 == m0, wc_tx_ms, now)) return;
    wc_tx_ms = now;
    wc_cam c;
    for (int i = 0; i < 3; i++) c.eye[i] = rdf(m0 + 0x40 + 4u * (unsigned)i), c.up[i] = rdf(m0 + 0x50 + 4u * (unsigned)i), c.tgt[i] = rdf(m0 + 0x60 + 4u * (unsigned)i);
    c.fov = rdf(m0 + 0x3C);
    if (!wc_finite(&c)) return;
    uint8_t b[WC_LEN];
    wc_enc(b, ++wc_tx_seq, &c);
    if (w1_send_ext(&N.w, CP_W1_CAM, b, WC_LEN)) {
        if (++WCS.tx == 1 || WCS.tx % 600 == 0)
            WS_LOG("watchcam: the other human is switched off: this PC's camera goes to the watching PC (%u sent; eye (%.0f,%.0f,%.0f) looking %.0f deg)", WCS.tx, c.eye[0], c.eye[1], c.eye[2], (double)wc_yaw_deg(&c));
    } else WCS.tx_fail++;
}
/* the watching PC: a camera from the active PC (op_ext, logic thread) */
static void watchcam_rx(const uint8_t *b, unsigned len)
{
    if (!wc_rx(&WCR, b, len, GetTickCount())) return;
    InterlockedIncrement(&wc_pub_seq);
    wc_pub = WCR.rx;
    wc_pub_ms = WCR.rx_ms;
    InterlockedIncrement(&wc_pub_seq);
    if (WCR.got == 1) WS_LOG("watchcam: the first camera of the active PC arrived (eye (%.0f,%.0f,%.0f) looking %.0f deg)", WCR.rx.eye[0], WCR.rx.eye[1], WCR.rx.eye[2], (double)wc_yaw_deg(&WCR.rx));
}
static void watchlight_pass(int force_off);
static uint32_t watchcam_stock_vt_of(uint32_t obj)
{
    uint32_t vt = obj ? rd32(obj) : 0;
    return wc_vt && vt == (uint32_t)(uintptr_t)wc_vt ? wc_stock_vt : vt;
}
static void watchcam_revert(void)
{
    watchlight_pass(1);   /* the zone managers' own targets back too */
    uint32_t o = wc_obj;
    if (wc_vt && o && rd32(o) == (uint32_t)(uintptr_t)wc_vt) wr32(o, wc_stock_vt), WCS.reverts++;
    wc_obj = 0;
}
/* the watching PC's manager 1 gets the private vtable while it watches (watch_pass, 250 ms) */
static void watchcam_pass(void)
{
    uint32_t m1 = g_solo_watch ? solo_w.mgr1 : 0;
    if (!f_watchcam || !m1) {
        if (wc_obj) watchcam_revert();
        return;
    }
    uint32_t vt = rd32(m1);
    if (wc_vt && vt == (uint32_t)(uintptr_t)wc_vt) return;
    char cn[64] = "";
    if (!vt || !classes_object_name(m1, cn, sizeof cn) || strcmp(cn, "uCameraManage")) return;
    if (wc_obj && wc_obj != m1) watchcam_revert();
    if (!wc_vt) {
        uint32_t *c = VirtualAlloc(NULL, WC_SLOTS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!c) return;
        for (int i = 0; i < WC_SLOTS; i++) c[i] = rd32(vt + 4u * (unsigned)i);
        wc_orig9 = (void (FC *)(unsigned))(uintptr_t)c[9];
        c[9] = (uint32_t)(uintptr_t)wc_wrap9;
        wc_vt = c, wc_stock_vt = vt;
    } else if (vt != wc_stock_vt) {
        return;   /* another vtable than the copy's: left alone */
    }
    wc_obj = m1;
    wr32(m1, (uint32_t)(uintptr_t)wc_vt);
    WCS.installs++;
    WS_LOG("watchcam: this PC WATCHES: camera manager 1 (%08x) shows the watched human's own camera from the active PC (after its stock update; stale after %u ms = the stock camera)", (unsigned)m1, WC_STALE_MS);
}


/* ---- rev1 live 14 (watchcam1.h wl_step, task 2): the watched human's light: the zone managers follow the watched human while this PC watches */
static volatile int f_watchlight = 1;
#define WL_N 3
static const uint32_t wl_off[WL_N] = {0x40, 0x44, 0x48};   /* sGameSystemsHolder_Episode [0x116A7D4]: sGameFilters, sGameLights, the 0x1E0 zone manager */
static wl_ent WLE[WL_N];
static uint32_t wl_checked[WL_N];                          /* the manager whose class was checked (a sZoneManager child) */
static int wl_ok[WL_N];
static struct { unsigned sets, backs, lines; } WLS;
static int wl_is_zone_mgr(uint32_t m)
{
    char cn[96] = "";
    if (!classes_object_name(m, cn, sizeof cn)) return 0;
    return !strcmp(cn, "sGameFilters") || !strcmp(cn, "sGameLights") || !strcmp(cn, "sEffectColorCorrect");
}
static void watchlight_pass(int force_off)
{
    uint32_t holder = rd32(0x116A7D4u), watched = (!force_off && g_solo_watch) ? solo_w.watched : 0;
    for (int i = 0; i < WL_N; i++) {
        uint32_t m = holder ? rd32(holder + wl_off[i]) : 0;
        if (m && m != wl_checked[i]) wl_checked[i] = m, wl_ok[i] = wl_is_zone_mgr(m);
        if (m && !wl_ok[i]) m = 0;
        uint32_t cur = m ? rd32(m + 0x24) : 0, v = 0;
        if (!wl_step(&WLE[i], f_watchlight && !force_off, m, cur, watched, &v)) continue;
        if (!m || !wr32(m + 0x24, v)) continue;
        if (v == watched && watched) WLS.sets++;
        else WLS.backs++;
        if (++WLS.lines <= 40)
            WS_LOG("watchlight: zone manager +0x%02x (%08x) target %08x -> %08x (%s)", (unsigned)wl_off[i], (unsigned)m, (unsigned)cur, (unsigned)v,
                   v == watched && watched ? "the WATCHED human: its zone's lights / filters here" : "given back after the watch");
    }
}

/* fix021 rule 6 (logic thread, from the EV_HITNOTE drain): a blow of this PC's own human on a key whose client drives it / has not handed it back yet */
static void wt_host_blow_note(w1_key key, float dealt)
{
    wt_ent *e = f_handover ? wt_find(&WTH, key, 0) : NULL;
    if (!e || !e->open || dealt <= 0.0f) return;
    wt_host_blow(&WTH, key, dealt);
    WS.host_blows++;
    if (WS.host_blows <= 20) WS_LOG("handover: this PC's own blow on %u/%u (%s): %.0f, summed %.0f until the client's hand-back", key >> 8, key & 0xFF, e->active ? "a reverse replica" : "taken back", dealt, e->hd);
}

/* ---- the frame (logic thread, model lock, a level being played) */
static void watch_pass(uint32_t now)
{
    watch_tx(now);
    watchcam_tx(GetTickCount());
    if ((int32_t)(now - wt_last_pass_ms) < 250) return;
    wt_last_pass_ms = now;
    if (bound_role == W1_HOST)
        for (unsigned i = 0; i < WT_MAX; i++)
            if (WTH.e[i].used && (WTH.e[i].active || wt_fresh(&WTH.e[i], now))) watch_apply_one(&WTH.e[i], now);
    watch_visible_list(now);
    watch_map_pass();
    watchcam_pass();
    watchlight_pass(0);
}

static void watch_status(char *out, int size)
{
    int o = snprintf(out, (size_t)size, "world watch (watch1.h): watchrep %d, rloc %d, watchmap %d; this PC %s | client: rounds %u, messages %u (failed %u), records %u | host: messages %u (old %u, bad %u, full %u), records %u, reverse replicas made %u, taken back %u, ended %u; applied %u, snaps %u, kind 3 %u, kind 2 %u, kind refused %u, rearranged %u | visible: lists %u, units %u, hidden bits cleared %u, stale %u, now %ld | minimap: installs %u, scoped updates %u, reverts %u",
                     f_watchrep, f_rloc, f_watchmap, g_solo_watch ? "WATCHES" : "does not watch", WS.tx_rounds, WS.tx_msgs, WS.tx_fail, WS.tx_recs, WTH.rx_msgs, WTH.rx_old, WTH.rx_bad, WTH.full, WTH.rx_recs,
                     WTH.made, WTH.taken_back, WTH.ended, WS.applied, WS.snaps, WS.kind_made, WS.kind_back, WS.kind_fail, WS.rearr, WS.vis_lists, WS.vis_units, WS.vis_cleared, WS.vis_stale, (long)wv_n,
                     WS.map_installs, WS.map_scoped, WS.map_reverts);
    o += snprintf(out + o, (size_t)(size - o), "\n  watchcam %d: sent %u (failed %u) | received %u (old %u, bad %u, %s) | manager 1 installs %u, reverts %u, frames written %ld, stale frames %ld",
                  f_watchcam, WCS.tx, WCS.tx_fail, WCR.got, WCR.old, WCR.bad, wc_fresh(&WCR, GetTickCount()) ? "fresh" : "stale", WCS.installs, WCS.reverts, (long)WCS.writes, (long)WCS.stale);
    o += snprintf(out + o, (size_t)(size - o), "\n  watchlight %d: zone manager targets set to the watched human %u, given back %u", f_watchlight, WLS.sets, WLS.backs);
    o += snprintf(out + o, (size_t)(size - o), "\n  zkeep %d (zkeep1.h rule 7, client): lists %ld, kept replicas listed %ld, hidden bits cleared %ld, zone-pass resets skipped %ld",
                  f_zkeep, (long)ZK.lists, (long)ZK.listed_units, (long)ZK.cleared, (long)ZK.skipped);
    o += snprintf(out + o, (size_t)(size - o), "\n  handover %d (fix021, 0.2.1: reverse replica while the host's unit sleeps, taken back awake, the host's own blows summed): "
                  "host zone-pass resets of reverse replicas skipped %ld, own blows summed %u | client: hand-backs that waited for a grab %u",
                  f_handover, (long)ZK.skipped_host, WS.host_blows, LR.back_held);
    for (unsigned i = 0; i < WT_MAX && o < size - 100; i++) {
        wt_ent *e = &WTH.e[i];
        if (!e->used) continue;
        o += snprintf(out + o, (size_t)(size - o), "\n  %u/%u %s, last record (%.0f,%.0f,%.0f) hp %.0f, own blows summed %.0f (%s)", e->key >> 8, e->key & 0xFF, e->active ? "REVERSE REPLICA" : "own",
                      e->pos[0], e->pos[1], e->pos[2], e->hp, e->hd, e->open ? "open" : "settled");
    }
}
