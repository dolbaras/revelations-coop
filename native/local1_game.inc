/*
 * local1_game.inc - included by world1_game.c (after the hooks above, before the HOOKS table): rev1 live round 11, the enemies the host does not simulate.
 * The rule and the evidence are in local1.h; this is the engine side:
 *   - the enemy log (both PCs, every 250 ms in a level): a record's unit appears / is replaced / goes away / changes its kind, with hp, E1C, state type, position,
 *     and the zones of the own human and of the partner's puppet (unit+0xE88 -> +0x298 / +0x2A4, FUN_006EDF70): the evidence of why an enemy is gone on one PC;
 *   - three observing hooks: FUN_0044F510 (a unit put to sleep outside the own human's zone), FUN_00464320 (ArgCharaRearrange: a unit re-created without
 *     SpawnChara), FsmRemoveArrangeChara (a story removal);
 *   - the client's side of the rule (lr_step): replica / own AI / the death report (CP_W1_LOCAL) / the hand-back with the hp; the host's side (local_rx);
 *   - op_spawn's way to place a host enemy whose record is placed here without a unit (the direct Continue: FUN_00459190 says "already placed"): FUN_00464320.
 * Switches: `world set localrun 0` (the model before the fix: every enemy a replica, no report), `world set rearr 0`, `world set ewatch 0`; `world local` = the table.
 */
#include "local1.h"
#include "stage1.h"
#include "watch1.h"

static lr_t LR;
/* rev1 live 13 (watch1.h, task 3): the state lives here because local_rx / local_reset / hook_zpass use it; the functions are in watch1_game.inc */
static wt_host WTH;                    /* host: the client's local enemies (reverse replicas) */
static wt_tx WTX;                      /* client: the sender of its local enemies' records */
static volatile int f_watchrep = 1;    /* both: the watching PC keeps the replicas near the watched human visible (rule 4); host: reverse replicas (rule 2) */
static volatile int f_rloc = 1;        /* client: send the local enemies' records (rule 1) */
static volatile int f_watchmap = 1;    /* both: the watching PC's HUD minimap follows the watched human (rule 5) */
#define WV_MAX 24
/* the units the zone-pass hook un-hides: two buffers (rev1 live 17): the logic thread fills the one the engine thread does not read, then switches wv_cur, so
 * a kept replica is never missing from the list for a frame (its zone-leave reset is skipped by the same list) */
static volatile LONG wv_cur;
static volatile LONG wv_cnt[2];
static volatile uint32_t wv_unit[2][WV_MAX], wv_entry[2][WV_MAX];
static volatile uint8_t wv_keep[2][WV_MAX];   /* 1 = zkeep1.h rule 7 (the host simulates it): its zone-leave reset is skipped too; 0 = watch1.h rule 4 */
#define wv_n (wv_cnt[wv_cur])
#include "zkeep1.h"
static volatile int f_zkeep = 1;       /* client: rule 7 of zkeep1.h; `world set zkeep 0` = the model before the fix (the m16a1 hide) */
static volatile DWORD zk_pass_tid;     /* the engine thread inside FUN_00460250 (hook_zpass) */
static struct { volatile LONG listed_units, lists, skipped, cleared; } ZK;
static uint32_t zk_said[16];   /* the units whose first rule-7 listing was logged in this level instance (the log stays short) */
static unsigned zk_nsaid;
static int zk_listed_keep(uint32_t u)   /* engine thread: is the unit in the current list as a rule-7 replica, and does its entry still point at it */
{
    LONG c = wv_cur, n = wv_cnt[c & 1];
    for (LONG i = 0; i < n && i < WV_MAX; i++)
        if (wv_unit[c & 1][i] == u && wv_keep[c & 1][i]) return wv_entry[c & 1][i] && rd32(wv_entry[c & 1][i] + 4) == u;
    return 0;
}
static void watch_after_zpass(void);
static void hx_reset(void);            /* hitx1_game.inc (rev1 live 14) */
static void mo_reset_all(void);        /* motion1_game.inc (rev1 live 14) */
static void bg_reset(void);            /* bag1_game.inc (rev1 live 14) */
static volatile int f_localrun = 1;
static volatile int f_rearr = 1;
static volatile int f_ewatch = 1;
static volatile int f_watchzone;   /* LAB (default 0): the zone pass of the watched human while this PC watches (WH_ZPASS) */
static volatile int f_watchstage = 1;  /* rev1 live 12 (stage1.h): the watching PC streams the watched human's stage parts (WH_STAGEFL); 0 = before (the void) */
static struct {
    unsigned zleave, zleave_kill, rearr, rearr_enemy, rmarr, took, back, made_replica, kind_fail, reports_tx, rx, spent, killed, hp_set, refused, nothing, ops_rearr, ops_rearr_fail,
        ew_lines, zone_lines, kill_skipped, parked, zpass_scoped, zpass_said, owner_set, stage_said;
} LS;

typedef void (TC *zleave_fn)(void *sa, void *unit, int kill);
typedef void (TC *zpass_fn)(void *sa);
static zpass_fn orig_zpass;
typedef unsigned (TC *stagefl_fn)(void *sys);
static stagefl_fn orig_stagefl;
static st_t ST;
static volatile LONG st_own_seq, st_own_taken;   /* the engine thread's latest own codes for the logic thread's send */
static uint32_t st_own_codes[ST_N];
typedef void (TC *rearr_fn)(void *sa, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
static zleave_fn orig_zleave;
static rearr_fn orig_rearr;
static act2_fn orig_rmarr;

/* the state type of a unit's state machine (FUN_0049C430: [[unit+0xE80]+0x14]+0xC); 0xFFFFFFFF = none */
static uint32_t lr_state_type(uint32_t unit)
{
    uint32_t mgr = unit ? rd32(unit + WH_U_MACHINE) : 0, cur = mgr ? rd32(mgr + 0x14) : 0;
    return mgr ? (cur ? rd32(cur + 0xC) : 0) : 0xFFFFFFFFu;
}
/* zone 0 of a unit (FUN_006EDF70(+0xE88 move object, i = 0): +0x298 block, +0x2A4 unique); -1 / -1 = none */
static void lr_zone(uint32_t unit, int *zb, int *zu)
{
    uint32_t mo = unit ? rd32(unit + 0xE88) : 0;
    *zb = mo ? (int)rd32(mo + 0x298) : -1, *zu = mo ? (int)rd32(mo + 0x2A4) : -1;
}

/* the control kind setter through the unit's own vtable slot 60, only when it is the verified 0x6CD4E0 (as make_replica). rev1 live 12: the owner goes with
 * the kind (local1.h "the engine's kind rule"): unit+0xE7C (FUN_006C9FC0) and the record's +0xD0 first, the engine's FUN_00459880 way; the kind alone was
 * undone at the unit's next state change (l11a). 1 = done */
#define LR_U_OWNER 0xE7Cu      /* FUN_006C9FD0 / FUN_006C9FC0: the unit's owner slot; FUN_0049CB70 derives the kind from it at every state change */
#define LR_ENTRY_OWNER 0xD0u   /* sArrange record: the owner a rearrange (FUN_00464320) re-creates the unit with */
static int lr_set_kind(uint32_t entry, uint32_t unit, int kind)
{
    uint32_t fn = 0;
    if (!unit || !call_ok(WH_C_SETKIND) || !wh_unit_vfn(&MEM, unit, WH_VF_SETKIND, &fn) || fn != wh_sites[WH_C_SETKIND].addr) {
        LS.kind_fail++;
        return 0;
    }
    int own = capture_own_effective();
    int owner = lr_owner_for(kind, own, wh_replica_owner(own, 1 - g_own));
    ops_enter();
    wr32(unit + LR_U_OWNER, (uint32_t)owner);
    if (entry) wr32(entry + LR_ENTRY_OWNER, (uint32_t)owner);
    ((kind_fn)(uintptr_t)fn)((void *)(uintptr_t)unit, kind);
    ops_leave();
    LS.owner_set++;
    return rd32(unit + WH_U_KIND) == (uint32_t)kind && (int)rd32(unit + LR_U_OWNER) == owner;
}

/* ---- hooks (engine threads: memory reads and the log only) */
static void TC hook_zleave(void *self, void *unit, int kill)
{
    uint32_t u = (uint32_t)(uintptr_t)unit;
    if (g_active && u && enemy_unit(u)) {
        LONG n = InterlockedIncrement((volatile LONG *)&LS.zleave);
        if (kill & 0xFF) InterlockedIncrement((volatile LONG *)&LS.zleave_kill);
        w1_key key = 0;
        int has = n <= 80 && unit_key(u, &key);   /* the first calls only (a list scan per call) */
        if (n <= 80)
            wlog("enemy: zone leave (FUN_0044F510) %s%u/%u unit %08x kind %u hp %.0f E1C %08x state type %u kill %d pos (%.0f,%.0f,%.0f)", has ? "" : "?", key >> 8, key & 0xFF, (unsigned)u,
                 rd32(u + WH_U_KIND), rdf(u + WH_U_HP), rd32(u + WH_U_FLAGS), (unsigned)lr_state_type(u), kill & 0xFF, rdf(u + WH_U_POS), rdf(u + WH_U_POS + 4), rdf(u + WH_U_POS + 8));
        /* rev1 live 17 (zkeep1.h rule 7): the client's zone pass must not reset a replica the host still simulates (m16a1: its human crossed into 257/1 mid-fight) */
        if (zk_skip_reset(f_zkeep && g_client, zk_listed_keep(u), zk_pass_tid == GetCurrentThreadId(), kill, (unsigned)lr_state_type(u))) {
            LONG k = InterlockedIncrement(&ZK.skipped);
            if (k <= 40)
                wlog("zkeep: zone leave of %s%u/%u unit %08x (a replica the HOST still simulates) on this PC's zone pass: its reset SKIPPED, kept visible and hittable (E1C %08x, this "
                     "PC's human left the enemy's zone)", has ? "" : "?", key >> 8, key & 0xFF, (unsigned)u, rd32(u + WH_U_FLAGS));
            return;
        }
    }
    orig_zleave(self, unit, kill);
}

static void TC hook_rearr(void *self, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    orig_rearr(self, a, b, c, d);
    LONG n = InterlockedIncrement((volatile LONG *)&LS.rearr);
    if (!g_active || n > 80) return;   /* ArgCharaRearrange runs it every frame while its node lasts: only the first calls are looked at (each read is a system call) */
    uint32_t sa = (uint32_t)(uintptr_t)self;
    unsigned cnt = wh_list_count(&MEM, sa, WH_LIST_ENEMIES);
    for (unsigned i = 0; i < cnt; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_ENEMIES, i, &e) || e.k[0] != a || e.k[1] != b || e.k[2] != c || e.k[3] != d) continue;
        unsigned bl = 0, ix = 0;
        int has = wh_entry_key(&MEM, sa, &e, &bl, &ix);
        InterlockedIncrement((volatile LONG *)&LS.rearr_enemy);
        wlog("enemy: rearrange (FUN_00464320) %s%u/%u record %08x: unit %08x kind %u hp %.0f E1C %08x owner %d%s", has ? "" : "?", bl, ix, (unsigned)e.entry, (unsigned)e.unit,
                 e.unit ? rd32(e.unit + WH_U_KIND) : 0, e.unit ? rdf(e.unit + WH_U_HP) : 0.0f, e.unit ? rd32(e.unit + WH_U_FLAGS) : 0, (int)rd32(e.entry + 0xD0),
                 in_ops() ? " (the mod's own call)" : g_client ? " (the client's engine: the rule decides its kind)" : "");
        break;
    }
}

static unsigned TC hook_rmarr(void *self, void *param, void *x)
{
    uint32_t p = (uint32_t)(uintptr_t)param;
    LONG n = InterlockedIncrement((volatile LONG *)&LS.rmarr);
    if (g_active && p && n <= 40)
        wlog("enemy: FsmRemoveArrangeChara param +4 %08x +8 %u +0xC %u +0x10 %08x", rd32(p + 4), rd32(p + 8), rd32(p + 0xC), rd32(p + 0x10));
    return orig_rmarr(self, param, x);
}

/* LAB `world set watchzone 1`: while this PC watches the other human (its own human switched off by the story), the per-frame zone pass of sArrange runs
 * with the own slot answering the WATCHED human's slot (capture_own_scope, this thread only): the units of the watched human's zone are woken here, not the
 * ones around the switched-off own human (the l10e host: Jill's puppet in a void, the key zombies gone). Off by default until a live run shows its effect. */
static void TC hook_zpass(void *self)
{
    int sc = 0, own = g_own;
    if (f_watchzone && g_active && g_solo_watch && (own == 0 || own == 1)) {
        capture_own_scope(1 - own);
        sc = 1;
        LS.zpass_scoped++;
        if (LS.zpass_said != g_solo_enters && LS.zpass_said < 1000) {
            LS.zpass_said = g_solo_enters;
            wlog("enemy: zone pass in the WATCHED human's view (own slot %d answers %d while this PC watches; LAB watchzone)", own, 1 - own);
        }
    }
    zk_pass_tid = GetCurrentThreadId();   /* rev1 live 17: hook_zleave skips the reset of a kept replica only inside this pass */
    orig_zpass(self);
    zk_pass_tid = 0;
    if (sc) capture_own_scope(-1);
    watch_after_zpass();   /* rev1 live 13 (watch1.h rule 4) + rev1 live 17 (zkeep1.h rule 7): the listed replicas are shown again in this frame */
}

/* rev1 live 12, task 5 (stage1.h): FUN_004176B0 = the stage part codes under the own player (engine thread, every frame). While this PC WATCHES (its human
 * switched off by the story) it answers with the watched human's codes (sent by the other PC), so the watched human's stage is streamed here as on the active
 * PC (l12a / l12b: the watching PC never streamed stage 257 = the black void); otherwise the stock cast, and its codes go to the logic thread for the send. */
static unsigned TC hook_stagefl(void *self)
{
    uint32_t sys = (uint32_t)(uintptr_t)self;
    if (g_active && st_use_partner(&ST, f_watchstage, g_solo_watch)) {
        uint32_t c[ST_N];
        memcpy(c, ST.partner, sizeof c);
        if (wr(sys + 0x40, c, sizeof c)) {
            ST.used++;
            if (LS.stage_said != g_solo_enters && LS.stage_said < 1000) {
                LS.stage_said = g_solo_enters;
                wlog("stage: this PC WATCHES: it streams the watched human's stage parts %04x %04x %04x %04x (sent by the other PC), not its own human's floor", c[0], c[1], c[2], c[3]);
            }
            return 1;
        }
    }
    unsigned r = orig_stagefl(self) & 0xFF;
    if (r == 1 && g_active) {
        for (int i = 0; i < ST_N; i++) st_own_codes[i] = rd32(sys + 0x40 + 4u * (unsigned)i);
        InterlockedIncrement(&st_own_seq);
    }
    return r;
}

/* ---- the enemy log + the client's rule (logic thread, model lock held) */
typedef struct {
    uint32_t entry, unit, e1c, st;
    uint16_t key;
    uint8_t used, has_key, kind;
    float hp, pos[3];
} ew_ent;
#define EW_MAX 160
static ew_ent EW[EW_MAX];
static uint32_t ew_last_ms;
static int ew_zone_own[2] = {-2, -2}, ew_zone_partner[2] = {-2, -2};
static uint16_t ew_said[64];   /* keys whose "stays on this PC's AI" line was written in this level instance */
static unsigned ew_nsaid;

static void local_reset(void)
{
    memset(EW, 0, sizeof EW);
    lr_reset(&LR);
    ew_zone_own[0] = ew_zone_own[1] = ew_zone_partner[0] = ew_zone_partner[1] = -2;
    ew_nsaid = 0;
    st_reset(&ST);
    wt_host_reset(&WTH);   /* rev1 live 13: the reverse replicas belong to one level instance */
    hx_reset();            /* rev1 live 14: the finish table and the last HitInfos too */
    mo_reset_all();        /* rev1 live 14: and the motion tables */
    bg_reset();            /* rev1 live 14: and the partner's last bag */
    InterlockedExchange(&wv_cnt[0], 0);
    InterlockedExchange(&wv_cnt[1], 0);
    zk_nsaid = 0;
}

static ew_ent *ew_slot(uint32_t entry)
{
    ew_ent *fr = NULL;
    for (unsigned i = 0; i < EW_MAX; i++) {
        if (EW[i].used && EW[i].entry == entry) return &EW[i];
        if (!EW[i].used && !fr) fr = &EW[i];
    }
    if (fr) memset(fr, 0, sizeof *fr), fr->used = 1, fr->entry = entry;
    return fr;
}

#define EW_LOG(...) do { if (f_ewatch && ++LS.ew_lines <= 400) wlog(__VA_ARGS__); } while (0)

static void local_send(unsigned type, w1_key key, float hp, unsigned killer)
{
    uint8_t b[LR_MSG_LEN];
    unsigned n = lr_enc(b, type, key, hp, killer);
    if (!w1_send_ext(&N.w, CP_W1_LOCAL, b, n)) wlog("enemy: report %u for %u/%u NOT queued", type, key >> 8, key & 0xFF);
    else LS.reports_tx++;
}

static void local_pass(uint32_t now)
{
    if ((int32_t)(now - ew_last_ms) < 250) return;
    ew_last_ms = now;
    {   /* rev1 live 12 (stage1.h): the stage part codes under this PC's active human go to the other PC on change */
        LONG q = st_own_seq;
        if (q != st_own_taken) {
            st_own_taken = q;
            uint32_t c[ST_N];
            memcpy(c, st_own_codes, sizeof c);
            if (bound && st_own(&ST, c, g_solo_watch)) {
                uint8_t b[ST_MSG_LEN];
                unsigned n = st_enc(b, c);
                if (!w1_send_ext(&N.w, CP_W1_STAGE, b, n)) wlog("stage: the codes %04x %04x %04x %04x NOT queued", c[0], c[1], c[2], c[3]);
                else if (ST.tx <= 60) wlog("stage: this PC's human stands on stage parts %04x %04x %04x %04x: sent to the other PC", c[0], c[1], c[2], c[3]);
            }
        }
    }
    uint32_t sa = sarrange();
    if (!sa) return;
    uint32_t own_u = 0, par_u = 0;
    wh_player_by_slot(&MEM, (unsigned)g_own, &own_u);
    wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &par_u);
    float ox = own_u ? rdf(own_u + WH_U_POS) : 0.0f, oz = own_u ? rdf(own_u + WH_U_POS + 8) : 0.0f;
    {   /* the zones of the two humans (where the engine wakes / hides the units on this PC) */
        int ob, ou, pb, pu;
        lr_zone(own_u, &ob, &ou), lr_zone(par_u, &pb, &pu);
        if ((ob != ew_zone_own[0] || ou != ew_zone_own[1] || pb != ew_zone_partner[0] || pu != ew_zone_partner[1]) && ++LS.zone_lines <= 120)
            wlog("enemy: zones: this PC's human (slot %d, unit %08x) %d/%d, the partner's puppet (unit %08x) %d/%d", (int)g_own, (unsigned)own_u, ob, ou, (unsigned)par_u, pb, pu);
        ew_zone_own[0] = ob, ew_zone_own[1] = ou, ew_zone_partner[0] = pb, ew_zone_partner[1] = pu;
    }
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_ENEMIES);
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_ENEMIES, i, &e)) continue;
        ew_ent *w = ew_slot(e.entry);
        if (!w) break;
        if (!w->has_key) {
            unsigned b = 0, ix = 0;
            if (!wh_entry_key(&MEM, sa, &e, &b, &ix) || b > 255 || ix > 255) continue;
            uint32_t cls = 0;
            if (!wh_char_class(&MEM, sa, b, ix, &cls) || !wh_class_is_enemy(cls)) continue;
            w->key = W1_KEY(b, ix), w->has_key = 1;
        }
        uint32_t u = e.unit;
        unsigned kind = u ? rd32(u + WH_U_KIND) : 0;
        float hp = u ? rdf(u + WH_U_HP) : 0.0f;
        uint32_t e1c = u ? rd32(u + WH_U_FLAGS) : 0, st = u ? (rd32(u + 0xC) & 7) : 0;
        float pos[3] = {u ? rdf(u + WH_U_POS) : 0.0f, u ? rdf(u + WH_U_POS + 4) : 0.0f, u ? rdf(u + WH_U_POS + 8) : 0.0f};
        unsigned b = w->key >> 8, ix = w->key & 0xFF;
        if (u != w->unit) {
            if (!w->unit)
                EW_LOG("enemy %u/%u: unit %08x appeared: kind %u hp %.0f E1C %08x state %u pos (%.0f,%.0f,%.0f), record used up %u", b, ix, (unsigned)u, kind, hp, e1c, st, pos[0], pos[1], pos[2],
                       rd8(e.entry + WH_ENTRY_SPENT));
            else if (!u)
                EW_LOG("enemy %u/%u: unit %08x GONE (last: kind %u hp %.0f E1C %08x state %u pos (%.0f,%.0f,%.0f)), record used up %u, this PC's human at (%.0f,%.0f)", b, ix,
                       (unsigned)w->unit, w->kind, w->hp, w->e1c, w->st, w->pos[0], w->pos[1], w->pos[2], rd8(e.entry + WH_ENTRY_SPENT), ox, oz);
            else
                EW_LOG("enemy %u/%u: unit %08x replaced by %08x: kind %u hp %.0f E1C %08x pos (%.0f,%.0f,%.0f)", b, ix, (unsigned)w->unit, (unsigned)u, kind, hp, e1c, pos[0], pos[1], pos[2]);
        } else if (u && (kind != w->kind || ((e1c ^ w->e1c) & LR_E1C_HIDDEN) || ((hp > 0.0f) != (w->hp > 0.0f)))) {
            EW_LOG("enemy %u/%u: unit %08x kind %u -> %u, hp %.0f -> %.0f, E1C %08x -> %08x", b, ix, (unsigned)u, w->kind, kind, w->hp, hp, w->e1c, e1c);
        }
        w->unit = u, w->kind = (uint8_t)kind, w->hp = hp, w->e1c = e1c, w->st = st;
        memcpy(w->pos, pos, sizeof w->pos);
        if (!g_client || !f_localrun || !u || (kind != 2 && kind != 3)) continue;
        float dx = pos[0] - ox, dz = pos[2] - oz, dist = wt_local_dist(g_solo_watch, own_u ? sqrtf(dx * dx + dz * dz) : 1e9f);   /* rev1 live 13: a watching PC takes nothing */
        int alive = hp > 0.0f && (st == 1 || st == 2) && !(e1c & 0x80000000u);
        int a = lr_step(&LR, w->key, u, kind, alive, hp, dist, now);
        if (a == LR_MAKE_REPLICA || a == LR_HAND_BACK) {
            int ok = lr_set_kind(e.entry, u, 3);
            if (a == LR_HAND_BACK) LS.back++, local_send(LR_REC_HP, w->key, hp, W1_NO_SLOT);
            else LS.made_replica++;
            wlog("enemy %u/%u: unit %08x (kind %u) %s: the host simulates it (its records carry a live unit)%s%s", b, ix, (unsigned)u, kind,
                 a == LR_HAND_BACK ? "HANDED BACK to the host (hp sent)" : "made a replica", ok ? "" : ": the kind setter REFUSED", a == LR_HAND_BACK ? "" : " (the engine re-created it as own AI)");
        } else if (a == LR_MAKE_LOCAL) {
            int ok = lr_set_kind(e.entry, u, 2);
            LS.took++;
            wlog("enemy %u/%u: unit %08x runs on THIS PC's AI now (kind 3 -> 2%s): the host does not simulate it (no unit / hidden there), this PC's human is %.0f away", b, ix, (unsigned)u,
                 ok ? "" : " REFUSED", dist);
        } else if (a == LR_REPORT_DEATH) {
            local_send(LR_REC_DEATH, w->key, 0.0f, (unsigned)g_own);
            wlog("enemy %u/%u: unit %08x (run on this PC's AI) died here: reported to the host (its record goes used up there)", b, ix, (unsigned)u);
        } else if (a == LR_NONE && kind == 2) {
            lr_ent *le = lr_find(&LR, w->key, 0);
            if (le && le->local) {
                int seen = 0;
                for (unsigned k = 0; k < ew_nsaid; k++) seen |= ew_said[k] == w->key;
                if (!seen && ew_nsaid < 64) {
                    ew_said[ew_nsaid++] = w->key, LS.took++;
                    wlog("enemy %u/%u: unit %08x (re-created as own AI by this PC's engine) stays on THIS PC's AI: the host does not simulate it, this PC's human is %.0f away", b, ix, (unsigned)u, dist);
                }
            }
        }
    }
}

/* both: the stage part codes under the partner's active human (CP_W1_STAGE) */
static void stage_rx(const uint8_t *b, unsigned len)
{
    if (st_rx(&ST, b, len) && ST.rx <= 60) wlog("stage: the other PC's human stands on stage parts %04x %04x %04x %04x", ST.partner[0], ST.partner[1], ST.partner[2], ST.partner[3]);
}

/* the host's model record of a key (NULL = not known) */
static w1_enemy *local_model(w1_key key)
{
    for (unsigned i = 0; i < N.w.ne; i++)
        if (N.w.e[i].key == key) return &N.w.e[i];
    return NULL;
}

/* host: a model-ALIVE record whose unit is gone here: its batches carry ai 0xFFFF from now on (local1.h rule 1) */
static void local_host_parked(w1_key key)
{
    w1_enemy *me = local_model(key);
    if (me && me->st == W1_ALIVE && me->ai != LR_AI_NOUNIT) {
        w1_host_sample(&N.w, key, me->pos, me->yaw, me->hp, LR_AI_NOUNIT, me->e1c);
        if (++LS.parked <= 40) wlog("enemy %u/%u: no unit on the host any more (model alive): its records say \"no unit here\" (ai 0xFFFF)", key >> 8, key & 0xFF);
    }
}

/* host: a report of the client (CP_W1_LOCAL) */
static void local_rx(const uint8_t *b, unsigned len)
{
    unsigned type, killer;
    uint16_t key;
    float hp;
    LS.rx++;
    if (bound_role != W1_HOST || !lr_dec(b, len, &type, &key, &hp, &killer)) return;
    uint32_t sa = sarrange();
    wh_entry e;
    if (!sa || !wh_find_key(&MEM, sa, WH_LIST_ENEMIES, key >> 8, key & 0xFF, &e)) {
        wlog("enemy %u/%u: the client's report %u has no record here", key >> 8, key & 0xFF, type);
        return;
    }
    uint32_t u = e.unit;
    int alive_unit = u && rdf(u + WH_U_HP) > 0.0f && ((rd32(u + 0xC) & 7) == 1 || (rd32(u + 0xC) & 7) == 2);
    int hidden = alive_unit && wt_report_hidden(&WTH, key, (rd32(u + WH_U_FLAGS) & LR_E1C_HIDDEN) != 0);   /* rev1 live 13: a reverse replica counts as hidden (watch1.h rule 2) */
    int spent = rd8(e.entry + WH_ENTRY_SPENT);
    int d = lr_host_decide(type, alive_unit, hidden, spent, alive_unit ? rdf(u + WH_U_HP) : 0.0f, hp);
    float pt[3] = {u ? rdf(u + WH_U_POS) : 0.0f, u ? rdf(u + WH_U_POS + 4) : 0.0f, u ? rdf(u + WH_U_POS + 8) : 0.0f};
    if (d == LR_H_SPEND_KILL) {
        uint32_t fn = 0;
        if (wh_unit_vfn(&MEM, u, 0x3C, &fn) && fn == wh_sites[WH_C_VF15].addr && call_ok(WH_C_VF15)) {
            ops_enter();
            ((vf0_fn)(uintptr_t)fn)((void *)(uintptr_t)u);
            ops_leave();
            LS.killed++;
        }
    }
    if (d == LR_H_SPEND || d == LR_H_SPEND_KILL) {
        wr8(e.entry + WH_ENTRY_SPENT, 1);
        LS.spent++;
        int md = w1_host_death(&N.w, key, killer < 2 ? (int)killer : W1_NO_SLOT, 0, pt);
        wlog("enemy %u/%u: the client's PC killed it on its own AI (this PC has %s): record used up here%s, model death %s", key >> 8, key & 0xFF,
             !u ? "no unit for it" : hidden ? "only a hidden unit: killed" : "a dead unit", d == LR_H_SPEND_KILL ? " (the hidden unit killed)" : "", md >= 0 ? "sent" : "not known to the model");
    } else if (d == LR_H_SET_HP) {
        wlog("enemy %u/%u: handed back by the client with hp %.0f (here %.0f): taken", key >> 8, key & 0xFF, hp, rdf(u + WH_U_HP));
        wrf(u + WH_U_HP, hp);
        LS.hp_set++;
    } else if (d == LR_H_REFUSE) {
        LS.refused++;
        wlog("enemy %u/%u: the client's report %u REFUSED: this PC simulates it (unit %08x hp %.0f E1C %08x)", key >> 8, key & 0xFF, type, (unsigned)u, u ? rdf(u + WH_U_HP) : 0.0f, u ? rd32(u + WH_U_FLAGS) : 0);
    }
}

/* client: place the host's enemy whose record is placed here without a unit (op_spawn after SpawnChara returned at once: FUN_00459190 "already placed",
 * continue1 c1c: the direct Continue left the Ooze 2/0 a record without a unit). FUN_00464320 re-creates the unit from the record (not used up) the way
 * ArgCharaRearrange does; make_replica after it gives the kind. 1 = a unit is there now */
static int local_rearrange_record(uint32_t sa, unsigned block, unsigned index)
{
    wh_entry e;
    if (!f_rearr || !orig_rearr || !sa || !wh_find_key(&MEM, sa, WH_LIST_ENEMIES, block, index, &e) || e.unit || rd8(e.entry + WH_ENTRY_SPENT)) return 0;
    ops_enter();
    orig_rearr((void *)(uintptr_t)sa, e.k[0], e.k[1], e.k[2], e.k[3]);
    ops_leave();
    wh_entry e2;
    int ok = wh_find_key(&MEM, sa, WH_LIST_ENEMIES, block, index, &e2) && e2.unit;
    if (ok) LS.ops_rearr++;
    else LS.ops_rearr_fail++;
    wlog("enemy %u/%u: the host's SPAWN found the record placed here without a unit: re-created through FUN_00464320 (the rearrange): %s", block, index,
         ok ? "unit there" : "STILL no unit (the record's area is not loaded here?)");
    return ok;
}

static void local_status(char *out, int size)
{
    int o = snprintf(out, (size_t)size, "world local (local1.h): %s; took %u, handed back %u, made replica %u, kind refused %u, reports sent %u | host: reports %u, used up %u (hidden units killed %u), hp taken %u, refused %u | rearrange calls %u (enemy %u, the mod's own %u, failed %u), zone leave %u (kills %u), story removals %u, enemy log lines %u, DEATH left on own corpses %u, host records marked 'no unit' %u, zone passes in the watched view %u (watchzone %d), owner+kind changes %u | stage: watchstage %d, own codes sent %u, the other's received %u (refused %u), passes streamed for the watched human %u, the other's now %04x %04x %04x %04x",
                 f_localrun ? "ON" : "OFF (`world set localrun 1`)", LS.took, LS.back, LS.made_replica, LS.kind_fail, LS.reports_tx, LS.rx, LS.spent, LS.killed, LS.hp_set, LS.refused, LS.rearr,
                 LS.rearr_enemy, LS.ops_rearr, LS.ops_rearr_fail, LS.zleave, LS.zleave_kill, LS.rmarr, LS.ew_lines, LS.kill_skipped, LS.parked, LS.zpass_scoped, f_watchzone, LS.owner_set, f_watchstage, ST.tx, ST.rx, ST.rx_bad, ST.used, ST.partner[0], ST.partner[1], ST.partner[2], ST.partner[3]);
    for (unsigned i = 0; i < LR_MAX && o < size - 120; i++) {
        lr_ent *e = &LR.e[i];
        if (!e->used) continue;
        o += snprintf(out + o, (size_t)(size - o), "\n  %u/%u unit %08x local %u reported %u record %s%s, last hp %.0f", e->key >> 8, e->key & 0xFF, (unsigned)e->unit, e->local, e->reported,
                      e->have_rec ? (e->host_sim ? "simulated by the host" : "NOT simulated by the host") : "none", e->have_rec ? "" : " yet", e->last_hp);
    }
}
