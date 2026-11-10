/*
 * hitx1_game.inc - included by world1_game.c after watch1_game.inc: rev1 live round 14, the client's kills die by the game's own death on both PCs (hitx1.h).
 *   client: hook_apply packs the REAL HitInfo of the own human's blow on a replica (the PCHIT packing) into the event; the drain sends it as ext CP_W1_HITX
 *           (instead of the bare HIT) and keeps the last one per key; on the host's DEATH the replica is made local, its hp 0, and that HitInfo (attacker-less)
 *           is queued through the Raid applier 0x49AA50, so its own update picks the stock death (hitx_replica_die);
 *   host:   hitx_rx plays the client's HitInfo through the applier with the client's puppet as attacker / owner (hit queue -> vf46 -> Damage -> OnHit: the
 *           reaction, the death state, DeadState, the record used up, the story), the old Damage + OnHit only when the applier cannot be used;
 *   both:   hitx_pass finishes a dead enemy that no stock death reached (vf15 + used up, local_rx's SPEND_KILL way) and logs when the stock death came.
 * Switches: `world set hitx 0|1`, `world set rdeath 0|1`, `world set deadguard 0|1` (local1), `world set finish <ms>` (0 = off); `world hitx` = counters.
 */
#include "hitx1.h"

static volatile int f_hitx = 1;               /* both: the real HitInfo with the client's hit / the host plays it through the applier */
static volatile int f_rdeath = 1;             /* client: the replica dies by its own update on the host's DEATH */
static volatile int f_deadguard = 1;          /* client: local1 never takes a replica the host's DEATH came for (local1.h) */
static volatile int f_finish = (int)HX_FINISH_MS;
static hx_wd_t HXW;                           /* both: dead enemies waiting for their stock death */
static hx_last_t HXL;                         /* client: the own human's last HitInfo per key */
#define HX_SEEN 64
static uint16_t hx_seen_key[HX_SEEN];         /* keys whose stock death was logged in this level instance */
static unsigned hx_nseen;
static uint32_t hx_dead_ms[W1_MAX_ENEMIES];   /* when the model said DEAD (index of N.w.e), for the log */
static uint32_t hx_last_pass;
static struct {
    unsigned tx, tx_fail, tx_dead, rx, applier, direct, late, nounit, badlen, serial_bad, puppet_bad;
    unsigned rdeath_stock, rdeath_old, rdeath_last, rdeath_kind_fail, stock_seen, fin_unit, fin_rec, lab_applier;
} HS;

static void hx_reset(void)
{
    hx_wd_reset(&HXW);
    hx_last_reset(&HXL);
    hx_nseen = 0;
    memset(hx_dead_ms, 0, sizeof hx_dead_ms);
}

/* the unit FUN_006C92A0 returns for a serial: the first of the sCharaBase list [0x11695C8]+0x34 .. +0x38 (node +0x10 unit, +8 next) with +0xE2C == serial */
static uint32_t hx_serial_unit(uint32_t serial)
{
    uint32_t cb = rd32(0x11695C8u), n = cb ? rd32(cb + 0x34) : 0, end = cb ? rd32(cb + 0x38) : 0;
    for (unsigned i = 0; n && n != end && i < 4096; i++) {
        uint32_t u = rd32(n + 0x10);
        if (u && rd32(u + WH_U_SERIAL) == serial) return u;
        n = rd32(n + 8);
    }
    return 0;
}

static int hx_applier_ready(uint32_t unit) { return call_ok(WH_C_DMGRECV) && rd32(0x11695C8u) && unit && rd32(unit + 0xEA0); }

/* the Raid applier with this payload on the unit (logic thread): hit queue + vf46 + Damage + OnHit */
static void hx_apply(uint32_t unit, const uint8_t pay[HX_PAY], uint16_t aser, uint16_t oser, int slot)
{
    uint8_t msg[WH_DMGMSG_SIZE], comp[WH_RAIDCOMP_SIZE];
    wh_pchit_unpack(pay, aser, oser, msg);
    memset(comp, 0, sizeof comp);
    memcpy(comp + 0x50, &unit, 4);
    ops_enter();
    ((raid2_fn)(uintptr_t)wh_sites[WH_C_DMGRECV].addr)(comp, msg, slot);
    ops_leave();
}

/* a HitInfo -> the payload (the PCHIT packing) */
static void hx_pack_hit(const uint8_t hit[WH_HIT_SIZE], uint8_t pay[HX_PAY])
{
    uint8_t msg[WH_DMGMSG_SIZE];
    wh_dmgmsg_from_hit(hit, 0, 0, msg);
    wh_pchit_pack(msg, pay);
}

/* ---- client: the drain's hit (logic thread, model lock). 1 = sent as HITX (the bare HIT is not sent then) */
static int hitx_send(w1_key key, float dmg, const uint8_t pay[HX_PAY])
{
    if (!f_hitx) return 0;
    if (w1_enemy_state(&N.w, key) != W1_ALIVE) {   /* as w1_client_hit: the host's DEATH came first, the blow is moot */
        HS.tx_dead++;
        return 1;
    }
    uint8_t b[HX_LEN];
    hx_enc(b, key, dmg, pay);
    hx_last_put(&HXL, key, pay);
    if (!w1_send_ext(&N.w, CP_W1_HITX, b, HX_LEN)) {
        if (++HS.tx_fail <= 10) wlog("hitx: the hit on %u/%u NOT queued as HITX: the bare HIT goes", key >> 8, key & 0xFF);
        return 0;
    }
    if (++HS.tx <= 12) wlog("hitx: the own human's blow on replica %u/%u (damage %.0f here) sent with its HitInfo (HITX)", key >> 8, key & 0xFF, (double)dmg);
    return 1;
}

/* ---- host: a client's HITX (op_ext, logic thread, model lock) */
static void op_apply_hit(void *u, w1_key key, int attacker_slot, float damage, uint32_t hit_flags, const float pt[3]);
static void hitx_rx(const uint8_t *b, unsigned len)
{
    uint16_t key;
    float dmg;
    const uint8_t *pay;
    HS.rx++;
    if (bound_role != W1_HOST) return;
    if (!hx_dec(b, len, &key, &dmg, &pay)) {
        HS.badlen++;
        return;
    }
    int slot = 1 - g_own;
    uint32_t unit = unit_of_key(key), pup = 0;
    float hp0 = unit ? rdf(unit + WH_U_HP) : 0.0f;
    wh_player_by_slot(&MEM, (unsigned)slot, &pup);
    uint32_t su = pup ? hx_serial_unit((uint32_t)slot) : 0, pst = pup ? (rd32(pup + 0xC) & 7) : 0;
    int serial_ok = pup && su == pup && (pst == 1 || pst == 2);
    if (pup && su != pup) HS.serial_bad++;
    if (pup && su == pup && !serial_ok) HS.puppet_bad++;
    int path = hx_host_path(f_hitx, w1_enemy_state(&N.w, key) == W1_ALIVE, unit && hp0 > 0.0f, hx_applier_ready(unit) != 0, 1, serial_ok);
    w1_enemy *me = local_model(key);
    if (path == HX_P_LATE) {
        HS.late++;
        return;
    }
    if (path == HX_P_NOUNIT) {
        HS.nounit++;
        return;
    }
    if (me) me->last_hit = (uint8_t)slot;   /* the model's last damager, as rx_hit sets it */
    if (path == HX_P_APPLIER) {
        uint32_t e1c0 = rd32(unit + WH_U_FLAGS);
        hx_apply(unit, pay, (uint16_t)slot, (uint16_t)slot, slot);
        HS.applier++;
        if (HS.applier <= 40 || rdf(unit + WH_U_HP) <= 0.0f)
            wlog("hitx: the client's blow on %u/%u (unit %08x) played through the Raid applier with its puppet %08x as attacker: hp %.1f -> %.1f (the client's damage %.0f), E1C %08x -> %08x%s",
                 key >> 8, key & 0xFF, (unsigned)unit, (unsigned)pup, (double)hp0, (double)rdf(unit + WH_U_HP), (double)dmg, e1c0, rd32(unit + WH_U_FLAGS),
                 rdf(unit + WH_U_HP) <= 0.0f ? ": a KILLING blow, the stock death follows in the enemy's own update" : "");
        return;
    }
    float pt[3];
    uint32_t fl;
    memcpy(pt, pay, 12);
    memcpy(&fl, pay + 36 + (0x4C - 0x44), 4);
    HS.direct++;
    if (HS.direct <= 20)
        wlog("hitx: the client's blow on %u/%u NOT through the applier (switch %d, applier %s, hit queue %08x, serial %u -> %08x, puppet %08x state %u): the old Damage %.0f + OnHit",
             key >> 8, key & 0xFF, f_hitx, wh_state_name(H.state[WH_C_DMGRECV]), unit ? rd32(unit + 0xEA0) : 0, (unsigned)slot, (unsigned)su, (unsigned)pup, (unsigned)pst, (double)dmg);
    op_apply_hit(NULL, key, slot, dmg, fl, pt);
}

/* ---- client: the replica on the host's DEATH (op_kill). 1 = it dies by its own update now */
static int hitx_replica_die(w1_key key, uint32_t unit, int killer_slot, uint32_t hit_flags, const float pt[3])
{
    uint32_t sa = sarrange();
    wh_entry e;
    int kind = (int)rd32(unit + WH_U_KIND);
    int d = hx_replica_death(f_rdeath, kind, call_ok(WH_C_DMGRECV) && rd32(0x11695C8u), rd32(unit + 0xEA0) != 0);
    if (d != HX_D_STOCK || !sa || !wh_find_key(&MEM, sa, WH_LIST_ENEMIES, key >> 8, key & 0xFF, &e) || e.unit != unit) {
        HS.rdeath_old++;
        return 0;
    }
    if (!lr_set_kind(e.entry, unit, 2)) {   /* local (owner = the engine's own slot): FUN_006C9FE0 lets its update request the death state */
        HS.rdeath_kind_fail++;
        if (HS.rdeath_kind_fail <= 10) wlog("hitx: replica %u/%u unit %08x: the kind setter refused: the old death (Damage + OnHit)", key >> 8, key & 0xFF, (unsigned)unit);
        return 0;
    }
    uint8_t pay[HX_PAY];
    const uint8_t *last = hx_last_get(&HXL, key);
    int use_last = hx_use_last(killer_slot, g_own, last != NULL);
    if (use_last) memcpy(pay, last, HX_PAY), HS.rdeath_last++;
    else {
        uint8_t hit[WH_HIT_SIZE];
        wh_hit_build(hit, WH_HIT_VTBL, 0, 0, hit_flags, pt);
        hx_pack_hit(hit, pay);
    }
    float hp0 = rdf(unit + WH_U_HP);
    wrf(unit + WH_U_HP, 0.0f);   /* the queued HitInfo is read with hp <= 0: a death state (FUN_005B8530), not a reaction */
    g_allow_kill = 1;
    hx_apply(unit, pay, 0xFFFFu, 0xFFFFu, 1 - g_own);   /* attacker-less: OnHit runs no kill record (the credit is credit_kill's), vf46 counts no own stats */
    g_allow_kill = 0;
    if (rdf(unit + WH_U_HP) > 0.0f) wrf(unit + WH_U_HP, 0.0f);
    HS.rdeath_stock++;
    if (HS.rdeath_stock <= 40)
        wlog("hitx: the host's DEATH of %u/%u (killer slot %d): replica %08x made local, hp %.0f -> 0, %s HitInfo queued through the applier: its own update picks the stock death (E1C %08x)",
             key >> 8, key & 0xFF, killer_slot, (unsigned)unit, (double)hp0, use_last ? "the own human's last" : "the DEATH's", rd32(unit + WH_U_FLAGS));
    return 1;
}

/* ---- both: the finish (rule 3) and the stock-death log, every 250 ms (logic thread, model lock) */
static void hitx_pass(uint32_t now)
{
    if ((int32_t)(now - hx_last_pass) < 250) return;
    hx_last_pass = now;
    LR.no_dead_guard = !f_deadguard;
    uint32_t sa = sarrange();
    if (!sa) return;
    for (unsigned i = 0; i < N.w.ne && i < W1_MAX_ENEMIES; i++) {
        w1_enemy *me = &N.w.e[i];
        if (me->st != W1_DEAD) {
            hx_dead_ms[i] = 0;
            continue;
        }
        if (!hx_dead_ms[i]) hx_dead_ms[i] = now ? now : 1;
        wh_entry e;
        if (!wh_find_key(&MEM, sa, WH_LIST_ENEMIES, me->key >> 8, me->key & 0xFF, &e)) continue;
        uint32_t u = e.unit;
        float hp = u ? rdf(u + WH_U_HP) : 0.0f;
        uint32_t e1c = u ? rd32(u + WH_U_FLAGS) : 0;
        int spent = rd8(e.entry + WH_ENTRY_SPENT);
        if ((spent || (e1c & HX_E1C_DEAD)) && u) {
            int seen = 0;
            for (unsigned k = 0; k < hx_nseen; k++) seen |= hx_seen_key[k] == me->key;
            if (!seen && hx_nseen < HX_SEEN) {
                hx_seen_key[hx_nseen++] = me->key, HS.stock_seen++;
                wlog("hitx: %u/%u (unit %08x): the STOCK DEATH came here %u ms after the model's death: E1C %08x (dead bit %u), record used up %u, state type %u, kind %u",
                     me->key >> 8, me->key & 0xFF, (unsigned)u, (unsigned)(now - hx_dead_ms[i]), e1c, (e1c & HX_E1C_DEAD) ? 1u : 0u, (unsigned)spent, (unsigned)lr_state_type(u), rd32(u + WH_U_KIND));
            }
        }
        int a = hx_wd_step(&HXW, me->key, 1, u, hp, e1c, spent, now, (uint32_t)(f_finish > 0 ? f_finish : 0));
        if (a == HX_WD_FINISH_UNIT) {
            uint32_t fn = 0;
            int killed = 0;
            if (wh_unit_vfn(&MEM, u, 0x3C, &fn) && fn == wh_sites[WH_C_VF15].addr && call_ok(WH_C_VF15)) {
                ops_enter();
                ((vf0_fn)(uintptr_t)fn)((void *)(uintptr_t)u);
                ops_leave();
                killed = 1;
            }
            wr8(e.entry + WH_ENTRY_SPENT, 1);
            HS.fin_unit++;
            wlog("hitx: %u/%u (unit %08x hp %.0f E1C %08x): NO stock death %d ms after the model's death: finished (%s, record used up), the way local_rx's SPEND_KILL does",
                 me->key >> 8, me->key & 0xFF, (unsigned)u, (double)hp, e1c, f_finish, killed ? "vf15" : "vf15 NOT verified: left");
        } else if (a == HX_WD_FINISH_RECORD) {
            wr8(e.entry + WH_ENTRY_SPENT, 1);
            HS.fin_rec++;
            wlog("hitx: %u/%u: dead in the model, no unit here, its record not used up after %d ms: marked used up", me->key >> 8, me->key & 0xFF, f_finish);
        }
    }
}

/* LAB `world kill` through the applier (host): hp 0, a HitInfo with the slot's human (serial checked) or none queued: the stock death as for a real kill. 1 = done */
static int hitx_lab_kill(uint32_t unit, uint32_t att, int by, const float pt[3])
{
    if (!f_hitx || !hx_applier_ready(unit)) return 0;
    uint16_t ser = 0xFFFFu;
    if (att && by >= 0 && hx_serial_unit((uint32_t)by) == att) ser = (uint16_t)by;
    uint8_t hit[WH_HIT_SIZE], pay[HX_PAY];
    wh_hit_build(hit, WH_HIT_VTBL, 0, 0, 0, pt);
    hx_pack_hit(hit, pay);
    wrf(unit + WH_U_HP, 0.0f);
    g_allow_kill = 1;
    hx_apply(unit, pay, ser, ser, by >= 0 ? by : 1 - g_own);
    g_allow_kill = 0;
    HS.lab_applier++;
    return 1;
}

static void hitx_status(char *out, int size)
{
    snprintf(out, (size_t)size,
             "world hitx (hitx1.h): hitx %d, rdeath %d, deadguard %d (replicas refused after a DEATH %u), finish %d ms | client: HITX sent %u (not queued %u, moot after a DEATH %u); replica deaths: stock %u (the own last HitInfo %u), old %u, kind refused %u | host: HITX received %u (bad %u): through the applier %u, the old Damage + OnHit %u, late %u, no unit %u; the puppet's serial resolved elsewhere %u, puppet not alive %u | both: stock deaths seen %u, finished units %u, records %u; LAB kills through the applier %u",
             f_hitx, f_rdeath, f_deadguard, LR.dead_refused, f_finish, HS.tx, HS.tx_fail, HS.tx_dead, HS.rdeath_stock, HS.rdeath_last, HS.rdeath_old, HS.rdeath_kind_fail, HS.rx, HS.badlen, HS.applier,
             HS.direct, HS.late, HS.nounit, HS.serial_bad, HS.puppet_bad, HS.stock_seen, HS.fin_unit, HS.fin_rec, HS.lab_applier);
}
