/*
 * world1_game.c - the engine hooks of the shared world of Revelations 1 (Episode 1), the game side of world1.c / world1_net.c.
 *
 * What it does (notes\net-world-impl.md section 5, net-world-enemies-re.md, net-items-doors-re.md):
 *   SpawnChara 0x4688F0   host: reports a newly placed enemy that has a unit (key = SpawnChara's block, index); client: the game's own placement runs, with an owner
 *                         other than the engine's own slot, so the engine makes the unit a replica (kind 3) itself; the host's SPAWN places what is missing through
 *                         ops.spawn. Never suppressed (a suppressed placement leaves no entry, and scripts look units up by (block, index)), and an entry without a
 *                         unit (block of another area: SpawnChara deletes the unit and writes entry+4 = 0, PROVEN 0x46A609..0x46A62D) is never touched.
 *   ApplyHit 0x6CD270     client: the own human's blow on a replica (view control kind 3) is sent to the host as a HIT; the replica loses only hp-1 locally.
 *                         host: the own human's blow is noted for the kill credit
 *   OnHit 0x6CCF30        host: death of an enemy (hp <= 0) is reported with the killer slot; client: a replica never dies from its own blow (hp stays 1), it dies
 *                         when the host's DEATH arrives (ops.kill: Damage + OnHit with the killer's human as attacker, so the kill record of the own human runs)
 *   AddItem 0x5CA480      the own human's pickup of a world item: sent as PICKUP when the item left the list (the other PC takes it from the world, no bag)
 *   kill record 0x89ECA0  observed only (counters, log)
 *   SetScenarioFlag 0x721DE0  the story mirror (story1_game.inc, sf1.c): Episode 1 flags host -> guest (sets and clears), the guest's own sets as proposals,
 *                         a digest after each load; plus the Genesis rate and broken set pieces (notes\ep1-world.md). ON in a session (`world sf off`)
 *
 * OFF by default. The hooks are written when a network session has STARTED, with REV1_WORLD=1 (then they only observe: no model, nothing is blocked or sent) or by `world apply`.
 * Every hook checks the bytes first (world1_hooks.c) and leaves the game alone with a logged reason when they differ.
 *
 * Threads: the engine calls the hooks from its unit threads. They never touch the model or the session: they read engine memory, decide with atomics and put events into a small
 * queue; the frame callback (logic thread) drains the queue into the model and runs the model's timers. The model's callbacks (ops.*) run on the logic thread too.
 *
 *   doors (round 4)       the own human's opening of a uDoorBase (KnobHandOn -> OpeningPlayer) is announced; the other PC asks its closed door to open through the game's own
 *                         request 0x62E360 (OpeningAuto with the opener's side, the door closes by itself) - `world set doors 1`
 * NOT done (needs RE or a live session, listed in notes\net-world-hooks.md): enemy loot, removal of a replica / corpse on the client (the unit delete is not
 * identified), animation / yaw / sleep bit of a replica, enemy attacks on the remote human (hp of the puppet), the despawn hook on the host (a per-frame list watcher does it).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "r1.h"
#include "capture.h"
#include "net1.h"
#include "lobby_link.h"
#include "world1_net.h"
#include "world1_hooks.h"
#include "pickpend1.h"
#include "world1_game.h"
#include "downed1.h"
#include "sf1.h"
#include "rolebarrier1.h"   /* continue1: rolebarrier_shielded */
#include "hunk1.h"          /* allep1: Hunk (class, start gun) */
#include "ach1_game.h"   /* rel010 B10: the client's own kill also counts for its achievements */

#define TC __attribute__((thiscall))

typedef void (TC *spawn_fn)(void *self, unsigned block, unsigned index, int owner, int flags, char from_host, char filter);
typedef unsigned (TC *hit_fn)(void *self, void *hit);
typedef unsigned (TC *add_fn)(void *self, unsigned a, void *item, unsigned c);
typedef unsigned (TC *kill_fn)(void *self, unsigned cls, unsigned serial, unsigned killer, void *victim, void *event, char flag);
typedef unsigned (TC *flag_fn)(void *self, unsigned id, int value);
typedef void (TC *damage_fn)(void *unit, float dmg);
typedef void (TC *gone_fn)(void *mgr, void *item);
typedef void (TC *kind_fn)(void *unit, int kind);
typedef unsigned (TC *death87_fn)(void *coop, void *victim);
typedef void (TC *raid2_fn)(void *comp, void *msg, int slot);          /* Raid appliers: __thiscall(component; msg, slot), ret 8 */
typedef void (TC *vf0_fn)(void *unit);                                  /* cUnit::kill vf15 */
typedef void (TC *tgt_fn)(void *unit, const float *pos, const float *ang, int flag);
typedef void (TC *euler_fn)(void *unit, float *out);
typedef unsigned (TC *chg_fn)(void *mgr, uint32_t dti, uint32_t arg, void *args);
typedef uint32_t (TC *cur_fn)(void *mgr);

static spawn_fn orig_spawn;
static hit_fn orig_apply, orig_onhit;
static add_fn orig_add;
static kill_fn orig_kill;
static flag_fn orig_flag;
static death87_fn orig_death87;

/* ---------------------------------------------------------------- memory table of the pure part */
static int m_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    return r1_safe_read((const void *)(uintptr_t)a, out, n);
}
static int m_write(void *u, uint32_t a, const void *p, uint32_t n)
{
    (void)u;
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)(uintptr_t)a, p, n);
    VirtualProtect((void *)(uintptr_t)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)a, n);
    return 1;
}
static uint32_t m_alloc(void *u, uint32_t n)
{
    (void)u;
    return (uint32_t)(uintptr_t)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);   /* a 32-bit process: the address fits */
}
static void m_log(void *u, const char *line)
{
    (void)u;
    r1_log("%s", line);
}
static const wh_mem MEM = {m_read, m_write, m_alloc, m_log, NULL};

static void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wlog(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    r1_log("world: %s", b);
}

static unsigned rd32(uint32_t a)
{
    uint32_t v = 0;
    wh_rd32(&MEM, a, &v);
    return v;
}
static float rdf(uint32_t a)
{
    float v = 0;
    wh_rdf(&MEM, a, &v);
    return v;
}
static int wr(uint32_t a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, p, n, &got) && got == n;
}
static int wrf(uint32_t a, float v) { return wr(a, &v, 4); }
static unsigned rd8(uint32_t a)
{
    uint8_t v = 0;
    if (a) MEM.read(MEM.user, a, &v, 1);
    return v;
}
static int wr8(uint32_t a, unsigned v)
{
    uint8_t b = (uint8_t)v;
    return wr(a, &b, 1);
}
static int wr32(uint32_t a, uint32_t v) { return wr(a, &v, 4); }

/* ---------------------------------------------------------------- state */
static wh_t H;
static int inited, last_want;
static CRITICAL_SECTION mcs;          /* the model, its glue and the bind state: the logic thread (frame, notes, console) only */
static CRITICAL_SECTION qcs;          /* the event queue: any thread, held for a few instructions */
static pp_t g_pp;                     /* pickups watched after AddItem (pickpend1.h), under qcs */
static void pickpend_forget(void);
static w1n_t N;
static int bound, bound_role, bound_own = -1;
static cs_t *bound_cs;
static lk_t *tap_lk;
static volatile int g_active;         /* 1 = the model is bound: the hooks act. 0 = they only observe */
static volatile int g_client;         /* active role is the client */
static volatile int g_own;            /* the slot of the human on this PC (0 lead / 1 partner) */
static volatile int g_replica = 1;    /* client: the client's own enemy placements are made as REPLICAS (owner = the host's slot, kind 3); `world set replica 0` leaves them local (own AI) */
static volatile DWORD g_ops_tid;      /* the thread that is inside an ops.* callback (the hooks let its calls through untouched) */
static volatile int g_allow_kill;     /* ops.kill is running: OnHit of a replica may really kill it */
static int g_status_active;           /* net1_session_active() as the frame last saw it (the status must not take the net lock under the model lock) */
static char g_core_why[160];          /* why the model is not bound although it should be */
static unsigned last_epoch_seen = 0xFFFFFFFFu;
/* features from notes\raid-reuse.md ("take now"): each behind its own verified call and a switch (`world set <name> <v>`). They act only while the model is bound
 * (a started network session): the split screen and the single-player game never reach them. */
static volatile int f_credit = 1;     /* client: kill credit by a direct kill record 0x89ECA0(class, serial, own slot, 0, 0, 0), once per enemy key */
static volatile int f_despawn = 1;    /* client: DESPAWN removes the replica: 0 off, 1 vf15 0x8CC9D0 + entry+0xD8 = 1, 2 the game's cFallOutData applier 0x455710 when [0x116FFF4] != 0 */
static volatile int f_pchit = 1;      /* host: an enemy's blow on the partner's puppet is not applied but sent; client: applied to its own human through 0x49AA50 */
static volatile int f_interp = 0;     /* both: 1 = the cNetInterpMove gates are patched and replicas get interpolator targets 0x6CD6B0; 0 (default since world-downed: in wp1-wp3 the targets never moved a replica, direct writes did) = position writes + snap */
static volatile int f_doorlog = 1;    /* both: door state changes are logged (type id of the current state, 0x49C3E0) */
static volatile int f_doors = 1;      /* both: a door the own human opens (OpeningPlayerState) is announced and the partner opens its copy (0x62E360), its Closed closes the copy
                                         (0x62E3A0). ON by default since wp12 + wp13 (2026-11-08: both directions live, open and close); `world set doors 0` = off (lab) */
static volatile int f_doorstate = WH_DS_OPENING_AUTO;
static volatile int f_itemspent = 1;  /* both: an item the other PC took is marked used up (entry+0xD8 = 1) after 0x5D7D00 */
static volatile int f_vital;          /* both: hp of the puppet from the partner's STATE through 0x697CD0. OFF by default */
static volatile int f_death87 = 1;    /* host: the cDeathEnemy sender 0x87BC60 reports deaths of every path */
static volatile int f_yawidx = 1;     /* host: which euler component of 0x9CF880 is the heading (LIVE: check) */
static volatile int f_snap = 300;     /* client: a replica farther than this from the host's sample (or another floor) is teleported; 0 = off (wp1 live: it never caught up) */
static volatile int f_spawnfh;       /* client: the from_host argument of op_spawn's SpawnChara. 0 (ep1-probe e1p2): placed like the client's own script (SpawnChara with from_host 1
                                        clears the unit's scene id in an aCut (0x92) area, 0x4688F0 line 272; e1p2 live: the host's SPAWN of the Ooze (2,0) placed that way before the
                                        client's own ArgCharaSet, and the client's set_pcs_event saw the fight over at once and ended 1-1). `world set spawnfh 1` = the old way.
                                        continue1 (task 2, e1p6 / e1p9): after a Continue at CP 256 / 4 the host's Ooze 2/0 and zombie 0/0 came back by its checkpoint read without
                                        a SpawnChara call, the guest's own script placed nothing, and the host's SPAWN through SpawnChara(from_host 1) in aCut1100 made the
                                        current stage -1 (0x468DF3), so the engine deleted the unit at the end (block stage != -1: `placement registered without a unit`):
                                        no Ooze on the guest. With 0 the stage test is the real one and the unit stays a replica (kind 3 by the owner). */
static volatile int f_revive = 1;     /* client: a replica placed with hp 0 (no dead bit) gets the host's hp, so it is updated at all (wp1 live: 9-2 zombie 9/0 hp 0 vs 1800) */
/* host: a blow of the partner's PUPPET on an enemy. 0 (default) = dropped (ApplyHit's own "no hit" 0, as the engine does for an attacker of kind 3): the partner's blows count only
 * through his HIT from his PC (wp5: his own blow -> HIT -> `damage 14.0 ... caller <DLL>` -> death -> his credit). Otherwise each of his shots would hit twice (the puppet fires with
 * his pad here) and, while the game has taken the puppet back as the AI partner (wp3 / wp5: kind 2 ctl 2), the AI's kills were credited to him (wp1, wp4 W3). 1 = applied (old). */
static volatile int f_puppethits;
static wh_gates_t G;                  /* the two interpolator gates */
static wh_keyset credited;            /* client: enemy keys already credited in this level instance */
static uint32_t dsid[WH_N_DS];        /* door state type ids (0 = DTI not verified) */
static int dsid_done;
#define MAXDOORS 128
static struct { uint32_t k4[4]; uint32_t id; uint8_t own_open; } doors[MAXDOORS];   /* own_open: the own human's opening was announced; its Closed follows */
static unsigned ndoors;
static uint32_t last_door_ms, last_vital_ms;
static uint32_t last_despawn_ms;
#define WH_PASS_BLOCKS 1024u   /* ep1-probe: room of the host pass's block table (1-1 has more than 64) */

static struct {
    unsigned spawn_calls, spawn_enemy, spawn_replica, spawn_reported, spawn_ops, spawn_class_mismatch, key_overflow;
    unsigned spawn_nounit, spawn_none, kind_fixed, kind_refused, report_nounit;
    unsigned hits_seen, hits_sent, replica_saved, hits_noted;
    unsigned deaths_reported, kill_records, kill_records_new;
    unsigned pickups_sent, pickups_not_removed, pickups_unlisted, pickups_late, pickups_expired;
    unsigned flags_seen, flags_sent, keyflags_seen, keyflags_reported;
    unsigned ev_overflow, ev_dropped_loading, ev_dropped_nomodel;
    unsigned ops_update, ops_kill, ops_apply_hit, ops_item_gone, ops_item_missing, ops_flag, ops_despawn_stub, ops_noop;
    unsigned pass_spawn, pass_death, pass_despawn, pass_kept;
    unsigned credit_done, credit_dup, credit_refused;
    unsigned despawn_done, despawn_fallout, despawn_noentry, despawn_vf_refused, pass_parked;
    unsigned pchit_sent, pchit_dropped, pchit_applied, pchit_refused;
    unsigned interp_targets, interp_direct, yaw_samples;
    unsigned door_changes, door_logged, door_announced, door_set, door_set_refused, door_ignored, door_close_announced, door_closed, door_close_left;
    unsigned item_spent, vital_set, death87;
    unsigned snaps, snap_nomove, revived;
    unsigned puppet_dropped, puppet_credit_refused;
} S;

static int in_ops(void) { return g_ops_tid == GetCurrentThreadId(); }
static void ops_enter(void) { g_ops_tid = GetCurrentThreadId(); }
static void ops_leave(void) { g_ops_tid = 0; }

/* ---------------------------------------------------------------- event queue (hook threads -> logic thread) */
enum { EV_SPAWN = 1, EV_HITNOTE, EV_DEATH, EV_CLIENT_HIT, EV_PICKUP, EV_FLAG, EV_PCHIT };
typedef struct {
    int type;
    uint16_t key;
    uint32_t cls, hitflags, id;
    int owner, spawn_flags, slot, old;   /* old: EV_FLAG, the value before the store (-1 unknown, -2 not reported, -3 not mirrored) */
    float pos[3], hp, dmg;
    uint32_t k4[4];
    int32_t value;
    uint8_t blob[WH_PCHIT_LEN];   /* EV_PCHIT: the packed cDamageSyncData fields; EV_CLIENT_HIT (rev1 live 14): the own human's real HitInfo, the same packing */
    uint8_t have_blob;
} ev_t;
#define EVQ 256
static ev_t evq[EVQ];
static unsigned ev_head, ev_tail;

static void ev_push(const ev_t *e)
{
    EnterCriticalSection(&qcs);
    if (ev_head - ev_tail >= EVQ) {
        S.ev_overflow++;
    } else {
        evq[ev_head % EVQ] = *e;
        ev_head++;
    }
    LeaveCriticalSection(&qcs);
}
static int ev_pop(ev_t *e)
{
    int ok = 0;
    EnterCriticalSection(&qcs);
    if (ev_tail != ev_head) {
        *e = evq[ev_tail % EVQ];
        ev_tail++;
        ok = 1;
    }
    LeaveCriticalSection(&qcs);
    return ok;
}

/* ---------------------------------------------------------------- engine helpers */
static uint32_t sarrange(void) { return rd32(WH_SARRANGE_PTR); }

/* the model key (block << 8 | index) of an enemy unit; 0 = not placed by sArrange or out of the key range */
static int unit_key(uint32_t unit, w1_key *key)
{
    uint32_t sa = sarrange();
    wh_entry e;
    unsigned b, i;
    if (!sa || !wh_find_unit(&MEM, sa, WH_LIST_ENEMIES, unit, &e) || !wh_entry_key(&MEM, sa, &e, &b, &i)) return 0;
    if (b > 255 || i > 255) {
        S.key_overflow++;
        return 0;
    }
    *key = W1_KEY(b, i);
    return 1;
}

static uint32_t unit_of_key(w1_key key)
{
    uint32_t sa = sarrange();
    wh_entry e;
    if (!sa || !wh_find_key(&MEM, sa, WH_LIST_ENEMIES, key >> 8, key & 0xFF, &e)) return 0;
    return e.unit;
}

static int enemy_unit(uint32_t u) { return wh_class_is_enemy(wh_unit_class(&MEM, u)); }

static void hit_point(uint32_t hit, float pt[3])
{
    pt[0] = rdf(hit + 0x10), pt[1] = rdf(hit + 0x14), pt[2] = rdf(hit + 0x18);
}

/* a replica that was hit hard enough to die locally: remembered by victim until the ApplyHit hook that called OnHit has finished (units are hit from several threads) */
#define OVK 8
static volatile uint32_t ovk_unit[OVK];
static void ovk_mark(uint32_t u)
{
    for (int i = 0; i < OVK; i++)
        if (!ovk_unit[i] && InterlockedCompareExchange((volatile LONG *)&ovk_unit[i], (LONG)u, 0) == 0) return;
}
static int ovk_take(uint32_t u)
{
    int hit = 0;
    for (int i = 0; i < OVK; i++)
        if (ovk_unit[i] == u) ovk_unit[i] = 0, hit = 1;
    return hit;
}

#include "zone1_game.inc"    /* round 9: the story's zones count the partner only by his PC's report (puppet authority) */
#include "story1_game.inc"
#include "prompt1_game.inc"   /* ep1-probe E8: a story prompt pressed on one PC counts on both */
#include "solo1_game.inc"   /* ep1-solo: the story view of the partner actions, the watch of the solo part */

/* ---------------------------------------------------------------- hooks */
static int call_ok(int id);
/* a SpawnChara result made a replica (kind 3) when the engine did not do it itself. NULL-safe: an entry without a unit (a block of another area, PROVEN 0x46A609..0x46A62D)
 * is left alone; the setter is called only through the unit's own vtable slot 60 and only when that slot is the verified 0x6CD4E0. Any thread (no model access). */
static void make_replica(uint32_t sa, unsigned block, unsigned index)
{
    uint32_t unit = 0, fn = 0;
    int r = wh_spawn_result(&MEM, sa, block, index, &unit);
    if (r == WH_SP_NONE) {
        S.spawn_none++;
        return;
    }
    if (r == WH_SP_NOUNIT) {   /* placed, not instantiated here: there is no unit to touch (live run 1 crashed exactly here: SetKind(NULL)) */
        if (++S.spawn_nounit <= 40) wlog("block %u index %u: placement registered without a unit (block of another area): nothing to make a replica", block, index);
        return;
    }
    if (rd32(unit + WH_U_KIND) == 3) return;
    if (!call_ok(WH_C_SETKIND) || !wh_unit_vfn(&MEM, unit, WH_VF_SETKIND, &fn) || fn != wh_sites[WH_C_SETKIND].addr) {
        if (++S.kind_refused <= 20)
            wlog("replica block %u index %u unit %08x kind %u: the kind setter is not the verified 0x6CD4E0 (vtable slot %08x): left as it is", block, index, (unsigned)unit, rd32(unit + WH_U_KIND), (unsigned)fn);
        return;
    }
    ((kind_fn)(uintptr_t)fn)((void *)(uintptr_t)unit, 3);
    S.kind_fixed++;
    if (S.kind_fixed <= 40) wlog("replica block %u index %u unit %08x: kind set to %u", block, index, (unsigned)unit, rd32(unit + WH_U_KIND));
}

static void TC hook_spawn(void *self, unsigned block, unsigned index, int owner, int flags, char from_host, char filter)
{
    uint32_t cls = 0, sa = (uint32_t)(uintptr_t)self;
    int enemy = wh_char_class(&MEM, sa, block, index, &cls) && wh_class_is_enemy(cls);
    int mine = in_ops();
    S.spawn_calls++;
    if (enemy && g_active && g_client && g_replica && !mine) {
        /* the client's own script / arrangement: the game places the enemy as it always does (no NULL for the scripts that look it up later),
         * but with an owner other than the engine's own slot, so SpawnChara itself makes it a remote unit (kind 3, no AI); the host's SPAWN then finds it placed */
        int rowner = wh_replica_owner(capture_own_effective(), 1 - g_own);
        S.spawn_enemy++, S.spawn_replica++;
        if (S.spawn_enemy <= 400)
            wlog("SpawnChara block %u index %u owner %d -> %d (replica) flags %d fromHost %d filter %d class %08x", block, index, owner, rowner, flags, (int)from_host, (int)filter, (unsigned)cls);
        orig_spawn(self, block, index, rowner, flags, from_host, filter);
        make_replica(sa, block, index);
        return;
    }
    if (enemy) {
        S.spawn_enemy++;
        if (S.spawn_enemy <= 400)
            wlog("SpawnChara block %u index %u owner %d flags %d fromHost %d filter %d class %08x%s", block, index, owner, flags, (int)from_host, (int)filter, (unsigned)cls, mine ? " (from the model)" : "");
    }
    int pre = 0;
    wh_entry e;
    int report = enemy && g_active && !g_client && !mine && block < 256 && index < 256;
    if (report) pre = wh_find_key(&MEM, sa, WH_LIST_ENEMIES, block, index, &e);
    orig_spawn(self, block, index, owner, flags, from_host, filter);
    if (report && !pre) {
        uint32_t unit = 0;
        int r = wh_spawn_result(&MEM, sa, block, index, &unit);
        if (r == WH_SP_UNIT) {
            ev_t ev;
            memset(&ev, 0, sizeof ev);
            ev.type = EV_SPAWN, ev.key = W1_KEY(block, index), ev.cls = cls, ev.owner = owner, ev.spawn_flags = flags;
            ev.pos[0] = rdf(unit + WH_U_POS), ev.pos[1] = rdf(unit + WH_U_POS + 4), ev.pos[2] = rdf(unit + WH_U_POS + 8), ev.hp = rdf(unit + WH_U_HP);
            ev_push(&ev);
            S.spawn_reported++;
        } else if (r == WH_SP_NOUNIT) {
            S.report_nounit++;   /* registered for another area: not announced (the host's pass announces it when a unit appears) */
        }
    } else if (enemy && g_active && !g_client && !mine && (block >= 256 || index >= 256)) {
        S.key_overflow++;
    }
}

static unsigned TC hook_apply(void *self, void *hit)
{
    uint32_t victim = (uint32_t)(uintptr_t)self, h = (uint32_t)(uintptr_t)hit;
    if (g_active && !g_client && f_pchit && !in_ops() && wh_class_is_player(wh_unit_class(&MEM, victim)) && (int)rd32(victim + WH_U_SERIAL) == 1 - g_own) {
        /* the partner's puppet on the host: its hp lives on the partner's PC (raid-reuse.md 5.4). Never damaged here; an enemy's blow is sent (PCHIT) */
        static const uint32_t off[2] = {0x40, 0x44};
        int from_enemy = 0;
        w1_key ak = W1_NO_KEY;
        for (int i = 0; i < 2 && !from_enemy; i++) {
            uint32_t u = rd32(h + off[i]);
            if (u && enemy_unit(u)) from_enemy = 1, unit_key(u, &ak);
        }
        uint8_t hb[WH_HIT_SIZE], msg[WH_DMGMSG_SIZE];
        if (from_enemy && MEM.read(MEM.user, h, hb, WH_HIT_SIZE)) {
            ev_t ev;
            memset(&ev, 0, sizeof ev);
            ev.type = EV_PCHIT, ev.key = ak;
            wh_dmgmsg_from_hit(hb, 0, 0, msg);
            wh_pchit_pack(msg, ev.blob);
            ev_push(&ev);
            S.pchit_sent++;
        } else {
            S.pchit_dropped++;   /* not an enemy's blow (or unreadable): the puppet takes nothing on this PC */
            if (S.pchit_dropped <= 12) {   /* wp1 live: 92729 such blows in 40 min (9-2): who deals them (scene damage? an enemy's projectile without an owner?) */
                uint32_t a0 = rd32(h + 0x40), a1 = rd32(h + 0x44);
                wlog("blow on the partner's puppet not counted as an enemy's: +0x40 %08x class %08x, +0x44 %08x class %08x, flags +0x54 %08x", (unsigned)a0, (unsigned)wh_unit_class(&MEM, a0), (unsigned)a1,
                     (unsigned)wh_unit_class(&MEM, a1), rd32(h + 0x54));
            }
        }
        return 0;   /* ApplyHit's "no hit" result (its own early exits return 0, 0x6CD2C7) */
    }
    if (g_active && !in_ops() && rolebarrier_shielded() && wh_class_is_player(wh_unit_class(&MEM, victim)) && (int)rd32(victim + WH_U_SERIAL) == g_own) {
        /* continue1: a restart came and the session's barrier of the next level instance has not opened: this PC's own human takes no blow (ApplyHit's own
         * "no hit" 0; a grab starts from the hit result, so no grab either) */
        rolebarrier_shield_drop(0);
        return 0;
    }
    if (!g_active || !enemy_unit(victim)) return orig_apply(self, hit);
    int hslot = -1, hkind = 0;
    uint32_t hunit = 0;
    int human = wh_hit_human(&MEM, h, &hunit, &hslot, &hkind);
    int kind = (int)rd32(victim + WH_U_KIND);
    w1_key key;
    S.hits_seen++;
    if (wh_partner_blow_dropped(!g_client, human, hslot, (int)g_own, in_ops(), f_puppethits)) {
        S.puppet_dropped++;
        if (S.puppet_dropped <= 10)
            wlog("blow of the partner's puppet (unit %08x slot %d kind %d, %s) on enemy %08x dropped: his blows count through his own HIT", (unsigned)hunit, hslot, hkind,
                 capture_unit_human(hunit) ? "driven by his pad" : "NOT driven by his pad (AI / not captured)", (unsigned)victim);
        return 0;   /* ApplyHit's own "no hit" (0x6CD2C7) */
    }
    if (g_client && kind == 3 && human && hslot == g_own && !in_ops() && unit_key(victim, &key)) {
        /* the own human's blow on a replica: let the engine apply it, then keep the replica alive (OnHit refuses the death) and tell the host */
        float hp0 = rdf(victim + WH_U_HP);
        unsigned r = orig_apply(self, hit);
        float hp1 = rdf(victim + WH_U_HP);
        int overkill = ovk_take(victim);
        float dealt = overkill ? hp0 : (hp0 > hp1 ? hp0 - hp1 : 0.0f);
        if (dealt > 0.0f) {
            float send = overkill ? hp0 + 1.0f : dealt;   /* kill intent: more than the replica has */
            float keep = hp0 - dealt;
            if (keep < 1.0f) keep = 1.0f;
            if (keep != hp1) wrf(victim + WH_U_HP, keep);
            ev_t ev;
            memset(&ev, 0, sizeof ev);
            ev.type = EV_CLIENT_HIT, ev.key = key, ev.dmg = send, ev.hitflags = rd32(h + 0x54);
            hit_point(h, ev.pos);
            {   /* rev1 live 14 (hitx1.h): the real HitInfo goes along, so the host's enemy gets the blow through its own hit queue (reaction, death state) */
                uint8_t hb[WH_HIT_SIZE], msg[WH_DMGMSG_SIZE];
                if (MEM.read(MEM.user, h, hb, WH_HIT_SIZE)) wh_dmgmsg_from_hit(hb, 0, 0, msg), wh_pchit_pack(msg, ev.blob), ev.have_blob = 1;
            }
            ev_push(&ev);
            S.hits_sent++;
        }
        return r;
    }
    if (!g_client && human && hslot == g_own && hkind == 1 && unit_key(victim, &key)) {   /* the host's own human: credit if the blow kills */
        ev_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = EV_HITNOTE, ev.key = key, ev.slot = hslot;
        ev_push(&ev);
        S.hits_noted++;
    }
    return orig_apply(self, hit);
}

static unsigned TC hook_onhit(void *self, void *hit)
{
    uint32_t victim = (uint32_t)(uintptr_t)self, h = (uint32_t)(uintptr_t)hit;
    if (!g_active || !enemy_unit(victim) || rdf(victim + WH_U_HP) > 0.0f) return orig_onhit(self, hit);   /* the original returns at once while hp > 0 */
    int kind = (int)rd32(victim + WH_U_KIND);
    if (g_client && kind == 3) {
        if (g_allow_kill && in_ops()) return orig_onhit(self, hit);   /* the host said it died: really kill it */
        wrf(victim + WH_U_HP, 1.0f);                                   /* a replica never dies from the client's own blow: the host decides */
        ovk_mark(victim);
        S.replica_saved++;
        return 0;
    }
    w1_key key;
    if (!g_client && unit_key(victim, &key)) {
        int hslot = -1;
        ev_t ev;
        memset(&ev, 0, sizeof ev);
        uint32_t hunit = 0;
        ev.type = EV_DEATH, ev.key = key, ev.slot = wh_hit_human(&MEM, h, &hunit, &hslot, NULL) ? hslot : W1_NO_SLOT, ev.hitflags = rd32(h + 0x54);
        if (wh_death_credit_slot(ev.slot, W1_NO_SLOT, (int)g_own, in_ops(), ev.slot != W1_NO_SLOT && capture_unit_human(hunit)) != ev.slot) {   /* the partner's puppet killed with his pad NOT driving it (the AI): nobody is credited */
            S.puppet_credit_refused++;
            if (S.puppet_credit_refused <= 10) wlog("kill by the partner's puppet (unit %08x slot %d) while it is not driven by his pad: no kill credit", (unsigned)hunit, hslot);
            ev.slot = W1_NO_SLOT;
        }
        hit_point(h, ev.pos);
        ev_push(&ev);
        S.deaths_reported++;
    }
    return orig_onhit(self, hit);
}

static unsigned TC hook_add(void *self, unsigned a, void *item, unsigned c)
{
    uint32_t pack = (uint32_t)(uintptr_t)self, it = (uint32_t)(uintptr_t)item, owner = rd32(pack + WH_BAG_OWNER);
    uint32_t sa = sarrange();
    wh_entry e;
    int own = g_active && it && owner && wh_class_is_player(wh_unit_class(&MEM, owner)) && (int)rd32(owner + WH_U_SERIAL) == g_own && !in_ops();
    if (!own || !sa || !wh_find_unit(&MEM, sa, WH_LIST_ITEMS, it, &e)) {
        if (own && ++S.pickups_unlisted <= 20)   /* ammo from a box, a script's gift ...: not a world item, nothing to sync (wp9: the reason when a pickup sends nothing) */
            wlog("AddItem: the own human's bag %08x gets item %08x class %08x that is %s: not sent", (unsigned)pack, (unsigned)it, (unsigned)wh_unit_class(&MEM, it),
                 sa ? "not in the world item list" : "outside a level (no sArrange)");
        return orig_add(self, a, item, c);
    }
    uint32_t cls = wh_unit_class(&MEM, it);
    unsigned r = orig_add(self, a, item, c);
    wh_entry e2;
    int gone = !wh_find_unit(&MEM, sa, WH_LIST_ITEMS, it, &e2) || (rd32(it + WH_U_FLAGS) & 0x80000000u);   /* FUN_005D7D00 marks the item for deletion (E1C bit 31) or the list drops it */
    if (gone) {
        ev_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = EV_PICKUP, ev.cls = cls;
        memcpy(ev.k4, e.k, sizeof ev.k4);
        ev_push(&ev);
        S.pickups_sent++;
        if (S.pickups_sent <= 40)
            wlog("item %08x class %08x key %08x/%08x/%u/%u taken by the own human (slot %d, bag %08x): PICKUP sent to the partner", (unsigned)it, (unsigned)cls, (unsigned)e.k[0], (unsigned)e.k[1],
                 (unsigned)e.k[2], (unsigned)e.k[3], (int)g_own, (unsigned)pack);
    } else {   /* wp9 live: the item leaves the list only after AddItem returned: watched by the frame (pickpend1.h), sent when it is gone */
        S.pickups_not_removed++;
        EnterCriticalSection(&qcs);
        int w = pp_add(&g_pp, it, cls, e.k, GetTickCount());
        LeaveCriticalSection(&qcs);
        if (S.pickups_not_removed <= 20)
            wlog("item %08x class %08x key %08x/%08x/%u/%u: added to the own bag %08x, still in the world list (E1C %08x): %s", (unsigned)it, (unsigned)cls, (unsigned)e.k[0], (unsigned)e.k[1],
                 (unsigned)e.k[2], (unsigned)e.k[3], (unsigned)pack, rd32(it + WH_U_FLAGS), w ? "watched, sent when it leaves the list" : "NOT watched (already watched / table full)");
    }
    return r;
}

static unsigned TC hook_kill(void *self, unsigned cls, unsigned serial, unsigned killer, void *victim, void *event, char flag)
{
    unsigned r = orig_kill(self, cls, serial, killer, victim, event, flag);
    S.kill_records++;
    if ((r & 0xFF) != 0) S.kill_records_new++;
    if (S.kill_records <= 60) {
        int slot = -1, kind = -1;
        int human = wh_hit_human(&MEM, (uint32_t)(uintptr_t)event, NULL, &slot, &kind);
        wlog("kill record class %08x serial %u victim %08x flags %08x by human slot %d kind %d -> %u", cls, serial, (unsigned)(uintptr_t)victim, event ? rd32((uint32_t)(uintptr_t)event + 0x54) : 0, human ? slot : -1,
             human ? kind : -1, r & 0xFF);
    }
    return r;
}

static unsigned TC hook_flag(void *self, unsigned id, int value)
{
    int old = story_hook_old(id);   /* the value before the store, for ids that may cross (story1_game.inc) */
    unsigned r = orig_flag(self, id, value);
    S.flags_seen++;
    if (old == -2 && S.flags_seen <= 60) wlog("scenario flag %u = %d%s", id, value, in_ops() ? " (from the other PC)" : "");
    if (old != -2) {   /* the logic thread decides and logs (story_local) */
        ev_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = EV_FLAG, ev.id = id, ev.value = value, ev.old = old;
        ev_push(&ev);
    }
    return r;
}

/* rev1 live 10 (task 2, SF 4864): AddItem (cBioItemPack_vf6, 0x5CB004) and FUN_005C8560 call FUN_00721F90(item code, 1): a key item's flag from the episode
 * setup's rKeyItem table is stored with FUN_00721920 directly, so the SetScenarioFlag hook above never saw the barn key's 4864 (l9c: the walk through the
 * checkpoint sphere gave none, the reunion and the tied man's door needed it). The same event as a SetScenarioFlag store: the host streams it, the guest
 * proposes it; the other PC applies it through SetScenarioFlag's trampoline. */
static flag_fn orig_keyflag;
static unsigned TC hook_keyflag(void *self, unsigned code, int value)
{
    uint32_t id = 0;
    int have = wh_keyitem_flag(&MEM, (uint32_t)(uintptr_t)self, code, &id);
    int old = have ? story_hook_old(id) : -2;
    unsigned r = orig_keyflag(self, code, value);
    S.keyflags_seen++;
    if (have && S.keyflags_seen <= 40)
        wlog("key item %08x: its scenario flag %u <- %d (FUN_00721F90, not through SetScenarioFlag; before %d)%s", code, (unsigned)id, value, old, old == -2 ? ": not mirrored (no story mirror now)" : "");
    if (have && old != -2) {   /* the logic thread decides and logs (story_local) */
        ev_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = EV_FLAG, ev.id = id, ev.value = value, ev.old = old;
        ev_push(&ev);
        S.keyflags_reported++;
    }
    return r;
}

#include "local1_game.inc"   /* rev1 live 11: the enemies the host does not simulate (local1.h), the enemy log, the rearrange */
#include "watch1_game.inc"   /* rev1 live 13: the watching PC shows the enemies around the watched human (watch1.h) */
#include "hitx1_game.inc"    /* rev1 live 14: the client's kills die by the game's own death on both PCs (hitx1.h) */
#include "motion1_game.inc"  /* rev1 live 14: a replica plays its owner's motions (mot1.h) */
#include "bag1_game.inc"     /* rev1 live 14: a puppet carries and holds its owner's guns (bag1.h) */

static unsigned TC hook_death87(void *self, void *victim)
{
    uint32_t v = (uint32_t)(uintptr_t)victim;
    w1_key key;
    if (g_active && !g_client && f_death87 && v && enemy_unit(v) && unit_key(v, &key)) {
        ev_t ev;   /* no killer hint here: the model keeps the one of the OnHit report (it came first) or credits the last damager */
        memset(&ev, 0, sizeof ev);
        ev.type = EV_DEATH, ev.key = key, ev.slot = W1_NO_SLOT;
        ev.pos[0] = rdf(v + WH_U_POS), ev.pos[1] = rdf(v + WH_U_POS + 4), ev.pos[2] = rdf(v + WH_U_POS + 8);
        ev_push(&ev);
        S.death87++;
    }
    return orig_death87(self, victim);
}

static const struct { int id; void *fn; void **orig; } HOOKS[WH_N_HOOKS] = {
    {WH_SPAWN, (void *)hook_spawn, (void **)&orig_spawn}, {WH_APPLYHIT, (void *)hook_apply, (void **)&orig_apply}, {WH_ONHIT, (void *)hook_onhit, (void **)&orig_onhit},
    {WH_ADDITEM, (void *)hook_add, (void **)&orig_add},   {WH_KILLREC, (void *)hook_kill, (void **)&orig_kill},   {WH_FLAG, (void *)hook_flag, (void **)&orig_flag},
    {WH_DEATH87, (void *)hook_death87, (void **)&orig_death87},
    {WH_ZONEHIT, (void *)hook_zhit, (void **)&orig_zhit},
    {WH_ENPART, (void *)hook_enpart, (void **)&orig_enpart}, {WH_PWARP, (void *)hook_pwarp, (void **)&orig_pwarp}, {WH_PATK, (void *)hook_patk, (void **)&orig_patk},
    {WH_PLACE, (void *)hook_place, (void **)&orig_place},    {WH_GSRCH, (void *)hook_gsrch, (void **)&orig_gsrch}, {WH_GSRST, (void *)hook_gsrst, (void **)&orig_gsrst},
    {WH_PCSACT, (void *)hook_pcsact, (void **)&orig_pcsact},
    {WH_KEYFLAG, (void *)hook_keyflag, (void **)&orig_keyflag},
    {WH_ZLEAVE, (void *)hook_zleave, (void **)&orig_zleave}, {WH_REARR, (void *)hook_rearr, (void **)&orig_rearr}, {WH_RMARR, (void *)hook_rmarr, (void **)&orig_rmarr},
    {WH_ZPASS, (void *)hook_zpass, (void **)&orig_zpass},
    {WH_STAGEFL, (void *)hook_stagefl, (void **)&orig_stagefl},
};

/* ---------------------------------------------------------------- model callbacks (logic thread, model lock held) */
static int call_ok(int id) { return H.state[id] == WH_ON; }

static void op_spawn(void *u, const w1_enemy_rec *r)
{
    (void)u;
    uint32_t sa = sarrange(), cls = 0;
    unsigned block = r->key >> 8, index = r->key & 0xFF;
    if (!sa || !orig_spawn) return;
    if (!wh_char_class(&MEM, sa, block, index, &cls) || (r->cls && cls != r->cls)) {   /* the two PCs do not load the same placement table: never place something else */
        S.spawn_class_mismatch++;
        wlog("SPAWN block %u index %u class %08x does not match the local record (%08x): not placed", block, index, (unsigned)r->cls, (unsigned)cls);
        return;
    }
    int rowner = wh_replica_owner(capture_own_effective(), 1 - g_own);   /* an owner other than the engine's own slot: SpawnChara makes it kind 3 itself */
    ops_enter();
    orig_spawn((void *)(uintptr_t)sa, block, index, rowner, r->spawn_flags, f_spawnfh ? 1 : 0, 0);   /* already placed (by the client's own script, as a replica): the engine returns at once */
    ops_leave();
    S.spawn_ops++;
    {   /* rev1 live 11 (continue1 open item): placed here without a unit (FUN_00459190 "already placed" after a direct reload): the rearrange re-creates it */
        uint32_t unit = 0;
        if (wh_spawn_result(&MEM, sa, block, index, &unit) == WH_SP_NOUNIT) local_rearrange_record(sa, block, index);
    }
    make_replica(sa, block, index);   /* NULL-safe; the old code called SetKind(entry->unit) without checking the unit: live run 1 crash */
}

static int interp_ready(void) { return f_interp && G.state[WH_G_TICK] == WH_ON && G.state[WH_G_TARGET] == WH_ON && G.state[WH_G_QUEUE] == WH_ON && call_ok(WH_C_TGT); }
/* the state id the interpolator's target setter tests (FUN_0049C430: [[unit+0xE80]+0x14]+0xC); 0xFFFFFFFF = no state machine */
static uint32_t interp_state(uint32_t unit)
{
    uint32_t mgr = rd32(unit + WH_U_MACHINE), cur = mgr ? rd32(mgr + 0x14) : 0;
    return mgr ? (cur ? rd32(cur + 0xC) : 0) : 0xFFFFFFFFu;
}

static void op_update(void *u, const w1_enemy_rec *r)
{
    (void)u;
    lr_on_record(&LR, r->key, r->ai, r->e1c, cs_now(bound_cs));   /* rev1 live 11: does the host simulate it (local1.h rule 1) */
    uint32_t unit = unit_of_key(r->key);
    if (!unit || rd32(unit + WH_U_KIND) != 3) {
        S.ops_noop++;
        return;
    }
    float hp = rdf(unit + WH_U_HP);
    if (!(hp > 0.0f) && f_revive && r->hp > 0.0f && !(rd32(unit + WH_U_FLAGS) & 0x80000000u)) {   /* placed with hp 0 here, alive on the host: never dead, so take the host's hp */
        wrf(unit + WH_U_HP, r->hp);
        hp = r->hp;
        S.revived++;
        if (S.revived <= 20) wlog("replica %u/%u unit %08x had hp 0 without the dead bit, the host says %.0f: hp taken over", r->key >> 8, r->key & 0xFF, (unsigned)unit, r->hp);
    }
    if (hp > 0.0f) {
        float cur[3] = {rdf(unit + WH_U_POS), rdf(unit + WH_U_POS + 4), rdf(unit + WH_U_POS + 8)};
        uint32_t mo = rd32(unit + 0xE88);
        if (mo && wh_snap_needed(cur, r->pos, (float)f_snap)) {
            /* the move object's teleport byte in the same pass as the position (PcsMainCharacterSetPos 0x771AF0, the console `tp`): not swept against the collision */
            uint32_t zero = 0;
            wr8(mo + 0x2B4, 1);
            wr(unit + WH_U_POS, r->pos, 12);
            wr(unit + 0x4C, &zero, 4);
            S.snaps++;
            if (S.snaps <= 40)
                wlog("replica %u/%u unit %08x snapped from (%.0f,%.0f,%.0f) to the host's (%.0f,%.0f,%.0f)", r->key >> 8, r->key & 0xFF, (unsigned)unit, cur[0], cur[1], cur[2], r->pos[0], r->pos[1], r->pos[2]);
        } else if (!mo && wh_snap_needed(cur, r->pos, (float)f_snap)) {
            S.snap_nomove++;   /* no move object: only the normal paths below */
        }
        if (interp_ready() && rd32(unit + 0xECC)) {   /* the interpolator of the unit exists (made for 0x8001/0x8003 classes, 0x6D0471): smooth movement and heading */
            float pos[4] __attribute__((aligned(16))) = {r->pos[0], r->pos[1], r->pos[2], 0.0f};
            float ang[4] __attribute__((aligned(16))) = {r->yaw, 0.0f, 0.0f, 0.0f};   /* as the Raid applier 0x49A200: {heading, pitch, 0, 0}, flag 1 */
            ops_enter();
            ((tgt_fn)(uintptr_t)wh_sites[WH_C_TGT].addr)((void *)(uintptr_t)unit, pos, ang, 1);
            ops_leave();
            S.interp_targets++;
        } else {
            wr(unit + WH_U_POS, r->pos, 12);
            S.interp_direct++;
        }
        if (r->hp > 0.0f && fabsf(r->hp - hp) > 0.01f) wrf(unit + WH_U_HP, r->hp);
    }
    S.ops_update++;
}

/* client: the own human's kill, credited once by the kill record itself (Raid cRankSyncData pattern 0x89A3B0): no HitInfo, so the flags block (exe+1396) never runs */
static void credit_kill(w1_key key, uint32_t unit)
{
    uint32_t mgr = rd32(0x116A7E4u), cls = wh_unit_class(&MEM, unit), serial = rd32(unit + WH_U_SERIAL);
    if (!cls) wh_char_class(&MEM, sarrange(), key >> 8, key & 0xFF, &cls);
    if (!mgr || !cls || H.state[WH_KILLREC] != WH_ON) {
        if (++S.credit_refused <= 20) wlog("kill credit for block %u index %u not written (rank manager %08x, class %08x, kill record hook %s)", key >> 8, key & 0xFF, (unsigned)mgr, (unsigned)cls, wh_state_name(H.state[WH_KILLREC]));
        return;
    }
    if (!wh_keyset_first(&credited, key)) {   /* a second call would count again (`Net_IsHost()` is 1 offline: the existing-record branch adds +0x758 once more) */
        S.credit_dup++;
        return;
    }
    /* the record credits "own" when the killer equals +0xE2C of the unit of FUN_00882D60() (the engine's own slot, which capture answers): pass exactly that slot */
    int killer = capture_own_effective();
    unsigned r = ((kill_fn)(uintptr_t)wh_sites[WH_KILLREC].addr)((void *)(uintptr_t)mgr, cls, serial, (unsigned)killer, NULL, NULL, 0);
    S.credit_done++;
    ach1_game_own_kill();   /* rel010 B10: the kill record alone never moves the client's +0x72 (id 14): the game's own setter through the achievement gate */
    if (S.credit_done <= 40)
        wlog("kill credit: block %u index %u class %08x serial %u killer slot %d (session slot %d%s) -> %u", key >> 8, key & 0xFF, (unsigned)cls, (unsigned)serial, killer, (int)g_own,
             killer != g_own ? ", DIFFERENT: capture_set_own is not wired (B3)" : "", r & 0xFF);
}

static void op_kill(void *u, w1_key key, int killer_slot, int credit_own, uint32_t hit_flags, const float pt[3])
{
    (void)u;
    if (!lr_kill_allowed(&LR, key)) {   /* rev1 live 11: an enemy run on this PC's AI died here already (local1.h rule 4): the host's DEATH changes nothing */
        LS.kill_skipped++;
        if (LS.kill_skipped <= 20) wlog("enemy %u/%u: the host's DEATH left alone: it ran on this PC's AI (its death was this PC's)", key >> 8, key & 0xFF);
        return;
    }
    lr_on_death(&LR, key);
    uint32_t unit = unit_of_key(key), att = 0;
    if (unit && hitx_replica_die(key, unit, killer_slot, hit_flags, pt)) {   /* rev1 live 14: the replica dies by its own update (hitx1.h rule 2) */
        S.ops_kill++;
        if (f_credit && credit_own) credit_kill(key, unit);
        return;
    }
    if (!unit || !call_ok(WH_C_DAMAGE)) {
        S.ops_noop++;
        return;
    }
    if (!f_credit && killer_slot >= 0 && killer_slot != W1_NO_SLOT) wh_player_by_slot(&MEM, (unsigned)killer_slot, &att);   /* old path: the record runs inside OnHit */
    uint8_t hit[WH_HIT_SIZE];
    wh_hit_build(hit, WH_HIT_VTBL, att, att, f_credit ? 0 : hit_flags, pt);   /* attacker 0: OnHit kills without a record (it returns before 0x89ECA0 when +0x40/+0x44 are 0) */
    ops_enter();
    g_allow_kill = 1;
    float hp = rdf(unit + WH_U_HP);
    if (hp > 0.0f) ((damage_fn)(uintptr_t)wh_sites[WH_C_DAMAGE].addr)((void *)(uintptr_t)unit, hp);
    ((hit_fn)(uintptr_t)wh_sites[WH_ONHIT].addr)((void *)(uintptr_t)unit, hit);   /* through the hook: the replica branch lets it pass because g_allow_kill is set */
    g_allow_kill = 0;
    ops_leave();
    S.ops_kill++;
    if (f_credit && credit_own) credit_kill(key, unit);
}

static void op_despawn(void *u, w1_key key)
{
    (void)u;
    uint32_t sa = sarrange();
    wh_entry e;
    if (!f_despawn) {
        S.ops_despawn_stub++;
        return;
    }
    if (!sa || !wh_find_key(&MEM, sa, WH_LIST_ENEMIES, key >> 8, key & 0xFF, &e)) {
        S.despawn_noentry++;
        return;
    }
    if (f_despawn == 2 && e.unit && rd32(0x116FFF4u) && rd32(e.unit + 0xE88) && call_ok(WH_C_FALLOUT)) {   /* the game's own path (0x6F0660 asserts the sTigerTrap singleton) */
        uint8_t msg[0x10];
        memset(msg, 0, sizeof msg);
        uint32_t serial = rd32(e.entry + 0xC);
        memcpy(msg + 8, &serial, 4);
        msg[4] = 2;
        ops_enter();
        ((raid2_fn)(uintptr_t)wh_sites[WH_C_FALLOUT].addr)(NULL, msg, 1 - g_own);   /* its first instruction loads ecx itself */
        ops_leave();
        S.despawn_fallout++;
        return;
    }
    if (e.unit) {
        uint32_t fn = 0;
        if (wh_unit_vfn(&MEM, e.unit, 0x3C, &fn) && fn == wh_sites[WH_C_VF15].addr && call_ok(WH_C_VF15)) {
            ops_enter();
            ((vf0_fn)(uintptr_t)fn)((void *)(uintptr_t)e.unit);   /* cUnit::kill: state 1/2 -> 3, the engine removes it */
            ops_leave();
        } else {
            if (++S.despawn_vf_refused <= 20) wlog("despawn block %u index %u: unit %08x slot 15 is %08x, not the verified 0x8CC9D0: unit left alone", key >> 8, key & 0xFF, (unsigned)e.unit, (unsigned)fn);
        }
    }
    wr8(e.entry + WH_ENTRY_SPENT, 1);   /* used up: SpawnChara never places it again in this level instance */
    S.despawn_done++;
    if (S.despawn_done <= 40)   /* ep1-probe e1p2 / e1p3: a host DESPAWN of the live Ooze ended the client's 1-1 (its fight check reads this used-up byte as "dead") */
        wlog("despawn block %u index %u applied: unit %08x (class %08x hp %.0f) removed, entry %08x marked used up", key >> 8, key & 0xFF, (unsigned)e.unit, e.unit ? (unsigned)wh_unit_class(&MEM, e.unit) : 0,
             e.unit ? (double)rdf(e.unit + WH_U_HP) : 0.0, (unsigned)e.entry);
}

static void op_apply_hit(void *u, w1_key key, int attacker_slot, float damage, uint32_t hit_flags, const float pt[3])
{
    (void)u;
    uint32_t unit = unit_of_key(key), att = 0;
    if (!unit || !call_ok(WH_C_DAMAGE)) {
        S.ops_noop++;
        return;
    }
    if (rdf(unit + WH_U_HP) <= 0.0f) return;   /* already dead */
    if (attacker_slot >= 0 && attacker_slot != W1_NO_SLOT) wh_player_by_slot(&MEM, (unsigned)attacker_slot, &att);
    uint8_t hit[WH_HIT_SIZE];
    wh_hit_build(hit, WH_HIT_VTBL, att, att, hit_flags, pt);
    ops_enter();
    ((damage_fn)(uintptr_t)wh_sites[WH_C_DAMAGE].addr)((void *)(uintptr_t)unit, damage);   /* ApplyHit would drop the remote attacker (view control 3): Damage + OnHit directly */
    ((hit_fn)(uintptr_t)wh_sites[WH_ONHIT].addr)((void *)(uintptr_t)unit, hit);             /* host branch of the OnHit hook reports a death with the attacker's slot */
    ops_leave();
    S.ops_apply_hit++;
}

static void op_item_gone(void *u, const w1_key4 *key, uint32_t cls, int taker_slot)
{
    (void)u;
    uint32_t sa = sarrange(), mgr = rd32(WH_ITEMMGR_PTR);
    wh_entry e;
    if (!sa || !mgr || !call_ok(WH_C_ITEMGONE)) {
        S.ops_noop++;
        return;
    }
    if (!wh_find_key4(&MEM, sa, WH_LIST_ITEMS, key->k, &e) || !e.unit) {   /* already taken by a script / not placed here */
        S.ops_item_missing++;
        if (S.ops_item_missing <= 20)
            wlog("item key %08x/%08x/%u/%u class %08x taken by the partner (slot %d): not in this PC's list (gone already / not placed): nothing to remove", (unsigned)key->k[0], (unsigned)key->k[1],
                 (unsigned)key->k[2], (unsigned)key->k[3], (unsigned)cls, taker_slot);
        return;
    }
    ops_enter();
    ((gone_fn)(uintptr_t)wh_sites[WH_C_ITEMGONE].addr)((void *)(uintptr_t)mgr, (void *)(uintptr_t)e.unit);
    ops_leave();
    S.ops_item_gone++;
    if (f_itemspent) wr8(e.entry + WH_ENTRY_SPENT, 1), S.item_spent++;   /* the tail of the Raid pickup applier 0x5D6110: the item never comes back (Continue, block change) */
    if (S.ops_item_gone <= 40)
        wlog("item %08x class %08x key %08x/%08x/%u/%u taken by the partner (slot %d): removed here through 0x5D7D00 (E1C now %08x, spent %u); no bag here gets it", (unsigned)e.unit, (unsigned)cls,
             (unsigned)key->k[0], (unsigned)key->k[1], (unsigned)key->k[2], (unsigned)key->k[3], taker_slot, rd32(e.unit + WH_U_FLAGS), rd8(e.entry + WH_ENTRY_SPENT));
}


static void doors_resolve(void)
{
    if (dsid_done) return;
    int all = 1;
    for (int i = 0; i < WH_N_DS; i++) {
        if (!wh_door_dti(&MEM, i, &dsid[i])) dsid[i] = 0, all = 0;
    }
    dsid_done = 1;
    if (!all)   /* wp1 live: none verified on either PC; the raw words tell which of the three checks fails ([dti] vtable, [dti+4] name, [dti+0x10] parent, [dti+0x1C] id) */
        for (int i = 0; i < WH_N_DS; i++)
            wlog("door state DTI %s at %08x: words %08x %08x .. +0x10 %08x .. +0x1C %08x (expected vtable %08x, name %08x, parent %08x)", wh_door_states[i].name, (unsigned)wh_door_states[i].dti, rd32(wh_door_states[i].dti),
                 rd32(wh_door_states[i].dti + 4), rd32(wh_door_states[i].dti + 0x10), rd32(wh_door_states[i].dti + 0x1C), (unsigned)WH_DTI_VTBL, (unsigned)wh_door_states[i].name_str, (unsigned)WH_DOORSTATE_PARENT);
    wlog("door state DTIs %s:Closed %08x KnobHandOn %08x OpeningPlayer %08x OpeningAuto %08x Opened %08x Gimmick %08x", all ? "verified" : "NOT all verified (0 = refused)", (unsigned)dsid[0], (unsigned)dsid[1],
         (unsigned)dsid[2], (unsigned)dsid[3], (unsigned)dsid[4], (unsigned)dsid[5]);
}

static const char *ds_name(uint32_t id)
{
    for (int i = 0; i < WH_N_DS; i++)
        if (id && dsid[i] == id) return wh_door_states[i].name;
    return "?";
}

static uint32_t cur_state(uint32_t unit)
{
    uint32_t mgr = rd32(unit + WH_U_MACHINE);
    if (!mgr || !rd32(mgr + 0x14) || !call_ok(WH_C_CURSTATE)) return 0;
    return ((cur_fn)(uintptr_t)wh_sites[WH_C_CURSTATE].addr)((void *)(uintptr_t)mgr);
}

/* LAB / the partner's opening: the game's own open request 0x62E360 (the FSM SetOpenDoor command) on a closed uDoorBase; the door's ClosedState update enters OpeningAuto
 * with the mode in its own pass (no ChangeState from here: with arg 0 OpeningAuto starts no motion, 0x629DC0). 1 = requested */
typedef void (TC *dooropen_fn)(void *door, int a, int b);
typedef void (TC *doorclose_fn)(void *door);   /* 0x62E3A0: ECX = door, no stack argument, plain ret */
static int door_request_open(uint32_t unit, uint8_t phase, const char *why)
{
    int a = 1, b = 1;
    if (!unit || !call_ok(WH_C_DOOROPEN)) return 0;
    wh_door_req_args(phase, &a, &b);
    ops_enter();
    ((dooropen_fn)(uintptr_t)wh_sites[WH_C_DOOROPEN].addr)((void *)(uintptr_t)unit, a, b);
    ops_leave();
    if (S.door_set < 40) wlog("door %08x: %s -> open request 0x62E360(side %d, %d): +0xFB4 %08x +0xFE0 %u", (unsigned)unit, why, a, b, rd32(unit + WH_DOOR_FB4), rd32(unit + 0xFE0));
    return 1;
}

static void op_door_set(void *u, const w1_key4 *key, uint32_t vtbl, uint8_t phase)
{
    (void)u;
    uint32_t sa = sarrange();
    wh_entry e;
    if (!f_doors) {
        S.door_ignored++;
        return;
    }
    doors_resolve();
    int want = f_doorstate;
    if (!sa || !wh_find_key4(&MEM, sa, WH_LIST_OBJECTS, key->k, &e) || !e.unit || !dsid[WH_DS_CLOSED] || (want == WH_DS_OPENED ? !dsid[want] || !call_ok(WH_C_CHGSTATE) : !call_ok(WH_C_DOOROPEN))) {
        if (++S.door_set_refused <= 40) wlog("door key %08x/%08x/%u/%u: partner opened it (%s), here REFUSED (unit %08x, request call %s)", (unsigned)key->k[0], (unsigned)key->k[1], (unsigned)key->k[2], (unsigned)key->k[3], ds_name(vtbl),
                                             (unsigned)(sa && wh_find_key4(&MEM, sa, WH_LIST_OBJECTS, key->k, &e) ? e.unit : 0), wh_state_name(H.state[WH_C_DOOROPEN]));
        return;
    }
    uint32_t now = cur_state(e.unit), mgr = rd32(e.unit + WH_U_MACHINE);
    if (vtbl == dsid[WH_DS_CLOSED]) {   /* the opener's door is closed again (wp9: our copy, opened by the request, would stay Opened): the close request 0x62E3A0 */
        if (wh_door_close_wanted(dsid, now) && call_ok(WH_C_DOORCLOSE)) {
            ops_enter();
            ((doorclose_fn)(uintptr_t)wh_sites[WH_C_DOORCLOSE].addr)((void *)(uintptr_t)e.unit);
            ops_leave();
            S.door_closed++;
            if (S.door_closed <= 40) wlog("door %08x: the partner's door is closed again, here %s -> close request 0x62E3A0: +0xFB4 %08x", (unsigned)e.unit, ds_name(now), rd32(e.unit + WH_DOOR_FB4));
        } else if (++S.door_close_left <= 40)
            wlog("door %08x: the partner's door is closed again, here it is %s (%08x): left as it is (close call %s)", (unsigned)e.unit, ds_name(now), (unsigned)now, wh_state_name(H.state[WH_C_DOORCLOSE]));
        return;
    }
    if (now != dsid[WH_DS_CLOSED]) {   /* only a closed door is opened; anything else is left to the scripts of this PC */
        if (++S.door_set_refused <= 40) wlog("door %08x: partner opened it (%s), here it is %s (%08x): left as it is", (unsigned)e.unit, ds_name(vtbl), ds_name(now), (unsigned)now);
        return;
    }
    if (want == WH_DS_OPENED) {   /* old lab path (`world set doorstate opened`) */
        ops_enter();
        unsigned ok = ((chg_fn)(uintptr_t)wh_sites[WH_C_CHGSTATE].addr)((void *)(uintptr_t)mgr, wh_door_states[want].dti, phase == 1 ? 2 : 1, NULL);
        ops_leave();
        S.door_set++;
        if (S.door_set <= 40) wlog("door %08x: partner opened it -> ChangeState(%s) = %u", (unsigned)e.unit, wh_door_states[want].name, ok & 0xFF);
        return;
    }
    char why[64];
    snprintf(why, sizeof why, "partner opened it (%s, side %u)", ds_name(vtbl), phase);
    if (door_request_open(e.unit, phase, why)) S.door_set++;
}

/* both: scan of the object list (doors): log the state changes, announce the own human's opening (KnobHandOn -> OpeningPlayer, or -> Opened in one scan) when doors are on */
static void door_pass(uint32_t now)
{
    if ((int32_t)(now - last_door_ms) < 100 || (!f_doorlog && !f_doors)) return;
    last_door_ms = now;
    doors_resolve();
    uint32_t sa = sarrange();
    if (!sa || !call_ok(WH_C_CURSTATE)) return;
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_OBJECTS);
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_OBJECTS, i, &e) || !e.unit || (wh_unit_class(&MEM, e.unit) & 0xF00F0000u) != 0x80060000u) continue;
        uint32_t id = cur_state(e.unit);
        if (!id) continue;
        unsigned d;
        for (d = 0; d < ndoors && memcmp(doors[d].k4, e.k, sizeof e.k); d++) {}
        if (d == ndoors) {
            if (ndoors >= MAXDOORS) continue;
            memcpy(doors[ndoors].k4, e.k, sizeof e.k), doors[ndoors].id = id, doors[ndoors].own_open = 0, ndoors++;
            continue;
        }
        if (doors[d].id == id) continue;
        uint32_t old = doors[d].id;
        doors[d].id = id;
        int doorish = !strcmp(ds_name(old), "?") ? strcmp(ds_name(id), "?") != 0 : 1;
        if (!doorish) continue;   /* other objects (lockers, switches): not logged */
        S.door_changes++;
        uint32_t fb4 = rd32(e.unit + WH_DOOR_FB4);
        if (f_doorlog && ++S.door_logged <= 300) {
            char cn[64] = "?";
            classes_object_name(e.unit, cn, sizeof cn);
            wlog("door unit %08x %s class %08x key %08x/%08x/%u/%u: %s (%08x) -> %s (%08x), +0xFB4 %08x", (unsigned)e.unit, cn, (unsigned)wh_unit_class(&MEM, e.unit), (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2],
                 (unsigned)e.k[3], ds_name(old), (unsigned)old, ds_name(id), (unsigned)id, (unsigned)fb4);
        }
        if (f_doors && wh_door_player_open(dsid, old, id)) {
            w1_key4 k4;
            uint32_t sv = 0;
            unsigned sp = 0, sver = 0;
            memcpy(k4.k, e.k, sizeof k4.k);
            w1_door_get(&N.w, &k4, &sv, &sp, &sver);   /* 0 / unchanged when the model has no record of this door yet */
            uint8_t ph = wh_door_phase_next((uint8_t)sp, fb4);
            int sent = w1_local_door(&N.w, &k4, id, ph);
            if (sent) S.door_announced++, doors[d].own_open = 1;
            if (S.door_announced <= 40) wlog("door unit %08x: opened by the own human here: %s to the partner (side %u, phase %02x)", (unsigned)e.unit, sent ? "announced" : "NOT announced", ph & 3u, ph);
        } else if (f_doors && wh_door_own_closed(dsid, old, id, doors[d].own_open)) {   /* the own human's door is closed again: the partner closes its copy */
            w1_key4 k4;
            uint32_t sv = 0;
            unsigned sp = 0, sver = 0;
            memcpy(k4.k, e.k, sizeof k4.k);
            w1_door_get(&N.w, &k4, &sv, &sp, &sver);
            uint8_t ph = wh_door_phase_next((uint8_t)sp, fb4);
            int sent = w1_local_door(&N.w, &k4, id, ph);
            doors[d].own_open = 0;
            if (sent) S.door_close_announced++;
            if (S.door_close_announced <= 40) wlog("door unit %08x: closed again after the own human's opening: %s to the partner (phase %02x)", (unsigned)e.unit, sent ? "announced" : "NOT announced", ph);
        }
    }
}

/* console: world items [radius] [class filter]: the world items of the list (sArrange+0xA8: key, unit, class, position, E1C, the spent byte) near the own human. E1C bit 0x4000 is set
 * by the uItem constructor 0x5D9350 on every item (uCharaBase_vf34 0x6CFEB3: such a unit gets no state manager) - it says nothing about taken / hidden; bit 31 = being deleted */
static void world_items(char *out, int size, float radius, const char *flt)
{
    uint32_t sa = sarrange(), me = 0;
    int o = 0;
    if (!sa) {
        snprintf(out, (size_t)size, "world items: no level");
        return;
    }
    wh_player_by_slot(&MEM, (unsigned)g_own, &me);
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_ITEMS), shown = 0;
    o += snprintf(out + o, (size_t)(size - o), "world items (own human %08x, radius %.0f, list %u entries; pickups sent %u, taken from the partner %u (missing %u)):", (unsigned)me, (double)radius, n, S.pickups_sent,
                  S.ops_item_gone, S.ops_item_missing);
    for (unsigned i = 0; i < n && o < size - 200; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_ITEMS, i, &e)) continue;
        char cn[64] = "-";
        if (e.unit) classes_object_name(e.unit, cn, sizeof cn);
        if (flt && flt[0] && !strstr(cn, flt)) continue;
        float px = e.unit ? rdf(e.unit + WH_U_POS) : 0, py = e.unit ? rdf(e.unit + WH_U_POS + 4) : 0, pz = e.unit ? rdf(e.unit + WH_U_POS + 8) : 0;
        float dx = me ? px - rdf(me + WH_U_POS) : 0, dz = me ? pz - rdf(me + WH_U_POS + 8) : 0, dist = sqrtf(dx * dx + dz * dz);
        if (radius > 0 && (!e.unit || dist > radius)) continue;
        shown++;
        o += snprintf(out + o, (size_t)(size - o), "\n  %08x %-22s key %08x/%08x/%u/%u pos (%.0f,%.0f,%.0f) dist %.0f E1C %08x spent %u", (unsigned)e.unit, cn, (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2],
                      (unsigned)e.k[3], (double)px, (double)py, (double)pz, (double)dist, e.unit ? rd32(e.unit + WH_U_FLAGS) : 0, rd8(e.entry + WH_ENTRY_SPENT));
    }
    if (o < size - 40) snprintf(out + o, (size_t)(size - o), "\n  %u shown", shown);
}

/* console: world doors [radius]: the uDoorBase objects of the list (unit, class, key, position, state, +0xFB4) near the own human */
static void world_doors(char *out, int size, float radius)
{
    uint32_t sa = sarrange(), me = 0;
    int o = 0;
    doors_resolve();
    if (!sa) {
        snprintf(out, (size_t)size, "world doors: no level");
        return;
    }
    wh_player_by_slot(&MEM, (unsigned)g_own, &me);
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_OBJECTS);
    o += snprintf(out + o, (size_t)(size - o), "world doors (own human %08x, radius %.0f; doors %d, request call %s, close call %s):", (unsigned)me, (double)radius, f_doors, wh_state_name(H.state[WH_C_DOOROPEN]),
                  wh_state_name(H.state[WH_C_DOORCLOSE]));
    for (unsigned i = 0; i < n && o < size - 200; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_OBJECTS, i, &e) || !e.unit || (wh_unit_class(&MEM, e.unit) & 0xF00F0000u) != 0x80060000u) continue;
        uint32_t id = cur_state(e.unit);
        if (!strcmp(ds_name(id), "?")) continue;   /* not a uDoorBase */
        float dx = me ? rdf(e.unit + WH_U_POS) - rdf(me + WH_U_POS) : 0, dz = me ? rdf(e.unit + WH_U_POS + 8) - rdf(me + WH_U_POS + 8) : 0, dist = sqrtf(dx * dx + dz * dz);
        if (radius > 0 && dist > radius) continue;
        char cn[64] = "?";
        classes_object_name(e.unit, cn, sizeof cn);
        o += snprintf(out + o, (size_t)(size - o), "\n  %08x %-26s key %08x/%08x/%u/%u pos (%.0f,%.0f,%.0f) dist %.0f state %s fb4 %08x E1C %08x", (unsigned)e.unit, cn, (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2], (unsigned)e.k[3],
                      rdf(e.unit + WH_U_POS), rdf(e.unit + WH_U_POS + 4), rdf(e.unit + WH_U_POS + 8), (double)dist, ds_name(id), rd32(e.unit + WH_DOOR_FB4), rd32(e.unit + WH_U_FLAGS));
    }
}

/* client: an enemy's blow on this PC's human, decided on the host (PCHIT) */
static void op_pc_hit(void *u, w1_key att, const uint8_t *payload, unsigned len)
{
    (void)u;
    uint32_t me = 0;
    if (rolebarrier_shielded()) {   /* continue1: the barrier after a restart has not opened here: the host's blow is not applied to this PC's human */
        rolebarrier_shield_drop(1);
        return;
    }
    if (!f_pchit || len != WH_PCHIT_LEN || !call_ok(WH_C_DMGRECV) || !wh_player_by_slot(&MEM, (unsigned)g_own, &me) || !rd32(me + 0xEA0) || !rd32(0x11695C8u)) {
        if (++S.pchit_refused <= 20) wlog("enemy blow from the host not applied (switch %d, len %u, applier %s, own human %08x)", f_pchit, len, wh_state_name(H.state[WH_C_DMGRECV]), (unsigned)me);
        return;
    }
    uint16_t aser = 0xFFFF;
    if (att != W1_NO_KEY) {
        uint32_t au = unit_of_key(att);
        if (au) aser = (uint16_t)rd32(au + WH_U_SERIAL);   /* the applier looks the attacker up by THIS PC's serial (0x6C92A0) */
    }
    uint8_t msg[WH_DMGMSG_SIZE], comp[WH_RAIDCOMP_SIZE];
    wh_pchit_unpack(payload, aser, aser, msg);
    memset(comp, 0, sizeof comp);
    memcpy(comp + 0x50, &me, 4);
    ops_enter();
    ((raid2_fn)(uintptr_t)wh_sites[WH_C_DMGRECV].addr)(comp, msg, 1 - g_own);   /* vf46 (damage by THIS PC's difficulty) -> Damage -> OnHit: the own human's agony / game over */
    ops_leave();
    S.pchit_applied++;
}

/* both: hp of the puppet (the partner's human on this PC) from the partner's STATE, through the Raid vital applier */
static void vital_pass(uint32_t now)
{
    if (!f_vital || (int32_t)(now - last_vital_ms) < 200 || !call_ok(WH_C_VITAL)) return;
    last_vital_ms = now;
    int pid = cs_remote_pid(bound_cs);
    cp_state st;
    uint32_t pu = 0;
    if (pid < 0 || cs_remote_sample(bound_cs, pid, now, &st) == CS_SAMPLE_NONE || st.hpmax <= 0 || !wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &pu)) return;
    float hp = (float)st.hp;
    if (fabsf(rdf(pu + WH_U_HP) - hp) < 1.0f) return;
    uint8_t msg[0x14], comp[WH_RAIDCOMP_SIZE];
    memset(msg, 0, sizeof msg), memset(comp, 0, sizeof comp);
    msg[4] = 2;
    uint32_t cap = rd32(pu + 0x1368), x = rd32(pu + 0x1374);   /* the two other fields are kept as they are */
    memcpy(msg + 8, &hp, 4), memcpy(msg + 0xC, &cap, 4), memcpy(msg + 0x10, &x, 4);
    memcpy(comp + 0x50, &pu, 4);
    ops_enter();
    ((raid2_fn)(uintptr_t)wh_sites[WH_C_VITAL].addr)(comp, msg, 1 - g_own);
    ops_leave();
    S.vital_set++;
}

static const w1_ops OPS = {
    NULL, NULL, NULL, op_spawn, op_update, op_kill, op_despawn, op_apply_hit, op_item_gone, op_door_set, story_op_flag, op_pc_hit, op_ext,
};

/* ---------------------------------------------------------------- the frame (logic thread) */
static void drain(void)
{
    ev_t e;
    while (ev_pop(&e)) {
        if (!bound) {
            S.ev_dropped_nomodel++;
            continue;
        }
        if (cs_phase(bound_cs) != CP_PHASE_PLAYING) {   /* a new level instance resets the model when it starts: the host's pass re-reads the list then */
            S.ev_dropped_loading++;
            continue;
        }
        switch (e.type) {
        case EV_SPAWN:
            w1_host_spawn(&N.w, e.key, e.cls, e.owner, e.spawn_flags, e.pos, e.hp);
            break;
        case EV_HITNOTE:
            w1_host_hit_note(&N.w, e.key, e.slot);
            break;
        case EV_DEATH:
            w1_host_death(&N.w, e.key, e.slot, e.hitflags, e.pos);
            break;
        case EV_CLIENT_HIT:
            if (!(e.have_blob && hitx_send(e.key, e.dmg, e.blob))) w1_client_hit(&N.w, e.key, e.dmg, e.hitflags, e.pos);   /* rev1 live 14: HITX, else the bare HIT */
            break;
        case EV_PICKUP: {
            w1_key4 k4;
            memcpy(k4.k, e.k4, sizeof k4.k);
            w1_local_pickup(&N.w, &k4, e.cls);
            break;
        }
        case EV_FLAG:
            story_local(e.id, e.old, e.value);
            break;
        case EV_PCHIT:
            w1_host_pchit(&N.w, e.key, e.blob, WH_PCHIT_LEN);
            break;
        }
    }
}

/* the host's pass over the placed enemies: spawns the hooks missed (cutscenes, direct calls), samples for the 15 Hz stream, deaths without a hook, and despawns */
static void host_pass(uint32_t now)
{
    static uint32_t last_pass_ms;
    if ((int32_t)(now - last_pass_ms) < 33) return;   /* ~30 Hz: the stream to the client is 15 Hz, and every read of engine memory is a system call */
    last_pass_ms = now;
    uint32_t sa = sarrange();
    if (!sa) return;
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_ENEMIES);
    /* ep1-probe e1p3 (live): the block table of 1-1 has more than 64 blocks; the old wh_block_table(.., 64) then read NONE, no unit got a key, the pass listed
     * nothing and despawned every known enemy every 0.5 s (the host's live Ooze 2/0 "gone" in its own model), and the client's despawn marked the Ooze entry used
     * up, which the client's fight check read as dead: the client ended 1-1 by itself. The table is read with room for 1024 blocks, again when its count or the
     * level changes or every 2 s (every read is a system call), and no DESPAWN goes out from a pass that could not read the table. */
    static wh_block blocks[WH_PASS_BLOCKS];
    static unsigned nb;
    static uint32_t blocks_sa, blocks_cnt, blocks_ms;
    static unsigned nb_warned, nb_logged;
    {
        uint32_t bl = rd32(sa + WH_SARR_BLOCKS), cnt = bl ? rd32(bl + 0x74) : 0;
        if (sa != blocks_sa || cnt != blocks_cnt || (int32_t)(now - blocks_ms) >= 2000) {
            int changed = sa != blocks_sa || cnt != blocks_cnt;
            nb = wh_block_table(&MEM, sa, blocks, WH_PASS_BLOCKS);
            blocks_sa = sa, blocks_cnt = cnt, blocks_ms = now;
            if (changed && nb_logged++ < 40) wlog("host pass: the block table of sArrange %08x: %u blocks, %u read (room %u)", (unsigned)sa, (unsigned)cnt, nb, WH_PASS_BLOCKS);
            if (!nb && cnt && nb_warned++ < 5) wlog("host pass: the block table (%u blocks) could not be read: no keys, no DESPAWN from this pass", (unsigned)cnt);
        }
    }
    w1_key present[W1_MAX_ENEMIES];
    unsigned np = 0;
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        unsigned b, idx;
        if (!wh_list_at(&MEM, sa, WH_LIST_ENEMIES, i, &e)) continue;
        if (!e.unit) {   /* placed, not instantiated (block of another area): still there unless used up, so no DESPAWN for it */
            uint32_t rc = 0;
            if (!rd8(e.entry + WH_ENTRY_SPENT) && wh_key_in_table(blocks, nb, &e, &b, &idx) && b < 256 && idx < 256 && wh_char_class(&MEM, sa, b, idx, &rc) && wh_class_is_enemy(rc) &&
                np < W1_MAX_ENEMIES) {
                present[np++] = W1_KEY(b, idx), S.pass_parked++;
                local_host_parked(W1_KEY(b, idx));   /* rev1 live 11: its records say "no unit here" (local1.h rule 1) */
            }
            continue;
        }
        uint32_t cls = wh_unit_class(&MEM, e.unit);
        if (!wh_class_is_enemy(cls) || !wh_key_in_table(blocks, nb, &e, &b, &idx)) continue;
        if (b > 255 || idx > 255) {
            S.key_overflow++;
            continue;
        }
        w1_key key = W1_KEY(b, idx);
        if (np < W1_MAX_ENEMIES) present[np++] = key;
        float pos[3] = {rdf(e.unit + WH_U_POS), rdf(e.unit + WH_U_POS + 4), rdf(e.unit + WH_U_POS + 8)}, hp = rdf(e.unit + WH_U_HP);
        int st = w1_enemy_state(&N.w, key);
        if (st == W1_UNKNOWN) {
            if (hp > 0.0f) {
                w1_host_spawn(&N.w, key, cls, g_own, 0, pos, hp);   /* owner / flags of the original call are unknown here: the host's own slot, 0 */
                S.pass_spawn++;
            }
        } else if (st == W1_ALIVE) {
            if (hp > 0.0f) {
                uint32_t mach = rd32(e.unit + WH_U_MACHINE);
                float yaw = 0.0f;
                if (f_interp && call_ok(WH_C_EULER)) {   /* the heading the interpolator of the replica gets (component f_yawidx of the euler angles) */
                    float eu[8] __attribute__((aligned(16))) = {0};
                    ((euler_fn)(uintptr_t)wh_sites[WH_C_EULER].addr)((void *)(uintptr_t)e.unit, eu);
                    yaw = eu[f_yawidx >= 0 && f_yawidx < 3 ? f_yawidx : 1];
                    if (!(yaw == yaw) || fabsf(yaw) > 1000.0f) yaw = 0.0f;
                    yaw = atan2f(sinf(yaw), cosf(yaw));   /* -pi..pi for the i16 of the stream */
                    S.yaw_samples++;
                }
                w1_host_sample(&N.w, key, pos, yaw, hp, wt_batch_ai(&WTH, key, lr_host_ai(mach ? rd32(mach + 0x3C) : 0)), rd32(e.unit + WH_U_FLAGS));   /* rev1 live 12: never the 0xFFFF sentinel for a real unit; live 13: "no unit here" for a reverse replica (watch1.h rule 2) */
            } else if (w1_host_death(&N.w, key, W1_NO_SLOT, 0, pos) >= 0) {
                S.pass_death++;
            }
        }
    }
    if (nb && (int32_t)(now - last_despawn_ms) >= 500) {   /* a known enemy that left the list is gone (only from a pass that could key the units) */
        last_despawn_ms = now;
        for (unsigned i = 0; i < N.w.ne; i++) {
            w1_key key = N.w.e[i].key;
            if (N.w.e[i].st != W1_ALIVE && N.w.e[i].st != W1_DEAD) continue;
            int there = 0;
            for (unsigned j = 0; j < np; j++) there |= present[j] == key;
            if (!there) {   /* not keyed in this pass: a fresh read by the key decides (the table is cached for 2 s; e1p3: a live Ooze was reported gone) */
                wh_entry le;
                if (wh_key_listed(&MEM, sa, key >> 8, key & 0xFF, &le)) {
                    if (++S.pass_kept <= 20) wlog("host pass: block %u index %u not keyed in this pass but still listed (unit %08x): no DESPAWN", key >> 8, key & 0xFF, (unsigned)le.unit);
                    continue;
                }
            }
            if (!there && w1_host_despawn(&N.w, key) > 0) {
                S.pass_despawn++;
                if (S.pass_despawn <= 40) {   /* ep1-probe: why a known enemy was not seen in the list (its entry by the key, as the client finds it) */
                    wh_entry fe;
                    int found = wh_find_key(&MEM, sa, WH_LIST_ENEMIES, key >> 8, key & 0xFF, &fe);
                    wlog("host pass: block %u index %u not in the list pass (%u keys): DESPAWN sent; by the key: %s unit %08x class %08x hp %.0f spent %u ids %08x/%08x/%u/%u", key >> 8, key & 0xFF, np,
                         found ? "entry" : "NO entry", found ? (unsigned)fe.unit : 0, found && fe.unit ? (unsigned)wh_unit_class(&MEM, fe.unit) : 0, found && fe.unit ? (double)rdf(fe.unit + WH_U_HP) : 0.0,
                         found ? rd8(fe.entry + WH_ENTRY_SPENT) : 0, found ? (unsigned)fe.k[0] : 0, found ? (unsigned)fe.k[1] : 0, found ? (unsigned)fe.k[2] : 0, found ? (unsigned)fe.k[3] : 0);
                }
            }
        }
    }
}

static void tap_note(void *u, const cs_note *n)
{
    (void)u;
    EnterCriticalSection(&mcs);
    if (bound) w1n_note(&N, n);
    if (bound && n->what == CS_N_EVENT && n->ev && n->ev->kind == CP_EV_SFPRE) story_pre_rx(n->ev->body, n->ev->len);   /* ep1-probe: the host's pre-load digest */
    LeaveCriticalSection(&mcs);
}

/* the model is bound while the hooks are in and a network session has started; returns 1 when the model is (still) bound */
static void model_sync(void)
{
    lk_t *lk = net1_link_peek();
    lk_info in;
    memset(&in, 0, sizeof in);
    if (lk && lk_session(lk)) lk_get_info(lk, &in);
    int core = call_ok(WH_C_DAMAGE) && H.state[WH_SPAWN] == WH_ON && H.state[WH_APPLYHIT] == WH_ON && H.state[WH_ONHIT] == WH_ON;
    int want_bound = lk && lk_session(lk) && in.started && in.role >= 0 && (last_want ? 1 : 0) && core;
    if (last_want && lk && in.started && !core && !g_core_why[0])
        snprintf(g_core_why, sizeof g_core_why, "the core hooks are not all ON (spawn %s, applyhit %s, onhit %s, damage call %s)", wh_state_name(H.state[WH_SPAWN]), wh_state_name(H.state[WH_APPLYHIT]),
                 wh_state_name(H.state[WH_ONHIT]), wh_state_name(H.state[WH_C_DAMAGE]));
    if (core) g_core_why[0] = 0;
    if (bound && (!want_bound || lk_session(lk) != bound_cs || (in.host ? W1_HOST : W1_CLIENT) != bound_role || in.role != bound_own)) {
        bound = 0, g_active = 0;
        wlog("model unbound");
    }
    if (!bound && want_bound) {
        bound_cs = lk_session(lk), bound_role = in.host ? W1_HOST : W1_CLIENT, bound_own = in.role;
        EnterCriticalSection(&qcs);   /* events of an earlier session or of the time before the bind belong to nobody */
        ev_tail = ev_head;
        LeaveCriticalSection(&qcs);
        w1n_init(&N, bound_cs, bound_role, bound_own, &OPS);
        story_bind();
        wh_keyset_clear(&credited);
        ndoors = 0;
        pickpend_forget();
        if (tap_lk != lk) {
            lk_set_note_tap(lk, tap_note, NULL);
            tap_lk = lk;
        }
        g_own = bound_own, g_client = bound_role == W1_CLIENT;
        last_epoch_seen = 0xFFFFFFFFu;
        bound = 1;
        g_active = 1;
        wlog("model bound: %s, this PC's human is slot %d (capture own slot answers %d)", bound_role == W1_HOST ? "HOST" : "CLIENT", bound_own, capture_own_effective());
    }
}

static void hooks_apply(void)
{
    int ok_pre[WH_N_HOOKS];
    for (int i = 0; i < WH_N_HOOKS; i++) {   /* trampolines first, so a thread that enters a hook the moment its site is written finds the original */
        ok_pre[i] = wh_prepare(&H, &MEM, HOOKS[i].id);
        if (ok_pre[i]) *HOOKS[i].orig = (void *)(uintptr_t)H.tramp[HOOKS[i].id];
    }
    uint8_t db[8];   /* downed1 (REV1_DOWNED=1 or the lab god mode) detours Damage 0x6CA830 at its entry: that exact jump is accepted, the world calls through it */
    if (downed1_damage_patch(db)) wh_set_alt(&H, WH_C_DAMAGE, db, 8);
    else wh_set_alt(&H, WH_C_DAMAGE, NULL, 0);
    for (int c = WH_N_HOOKS; c < WH_N_SITES; c++) wh_check(&H, &MEM, c);
    int core_ok = ok_pre[WH_SPAWN] && ok_pre[WH_APPLYHIT] && ok_pre[WH_ONHIT] && call_ok(WH_C_DAMAGE);
    if (!core_ok) {   /* all or nothing for the core: a half-hooked combat chain is worse than none */
        wlog("core hooks cannot all be written (see the refusals above): no hook is written");
        return;
    }
    for (int i = 0; i < WH_N_HOOKS; i++)
        if (ok_pre[i]) wh_apply(&H, &MEM, HOOKS[i].id, (uint32_t)(uintptr_t)HOOKS[i].fn);
    if (f_interp && call_ok(WH_C_TGT))
        for (int g = 0; g < WH_N_GATES; g++) wh_gate_apply(&G, &MEM, g);
}

static void gates_revert(void)
{
    for (int g = WH_N_GATES - 1; g >= 0; g--) wh_gate_revert(&G, &MEM, g);
}

static void hooks_revert(void)
{
    g_active = 0;
    gates_revert();
    watch_map_revert();   /* rev1 live 13: the stock minimap manager gets its own vtable back */
    watchcam_revert();    /* rev1 live 14: and camera manager 1 */
    for (int i = WH_N_HOOKS - 1; i >= 0; i--) wh_revert(&H, &MEM, HOOKS[i].id);
}

/* pickups watched after AddItem (pickpend1.h): the item unit still in the item list and not marked for deletion = listed; no sArrange = no level */
#define PP_MAX_MS 60000u   /* the delay between AddItem and the list removal is not known (a pickup message?): wide, the log line gives the age */
static int pp_listed_cb(void *u, uint32_t unit)
{
    (void)u;
    uint32_t sa = sarrange();
    wh_entry e;
    if (!sa) return -1;
    if (!wh_find_unit(&MEM, sa, WH_LIST_ITEMS, unit, &e)) return 0;
    return (rd32(unit + WH_U_FLAGS) & 0x80000000u) ? 0 : 1;
}
static void pp_sent_cb(void *u, const pp_ent *pe, uint32_t age)
{
    (void)u;
    ev_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_PICKUP, ev.cls = pe->cls;
    memcpy(ev.k4, pe->k, sizeof ev.k4);
    ev_push(&ev);
    S.pickups_sent++, S.pickups_late++;
    if (S.pickups_sent <= 40)
        wlog("item %08x class %08x key %08x/%08x/%u/%u left the world list %u ms after AddItem (taken by the own human, slot %d): PICKUP sent to the partner", (unsigned)pe->unit, (unsigned)pe->cls,
             (unsigned)pe->k[0], (unsigned)pe->k[1], (unsigned)pe->k[2], (unsigned)pe->k[3], (unsigned)age, (int)g_own);
}
static void pp_exp_cb(void *u, const pp_ent *pe, uint32_t age)
{
    (void)u;
    S.pickups_expired++;
    if (S.pickups_expired <= 20)
        wlog("item %08x class %08x key %08x/%08x/%u/%u still in the world list %u ms after AddItem: not sent (nothing was taken away)", (unsigned)pe->unit, (unsigned)pe->cls, (unsigned)pe->k[0],
             (unsigned)pe->k[1], (unsigned)pe->k[2], (unsigned)pe->k[3], (unsigned)age);
}
static void pickpend_pass(void)
{
    EnterCriticalSection(&qcs);
    if (pp_count(&g_pp)) pp_pass(&g_pp, GetTickCount(), PP_MAX_MS, pp_listed_cb, pp_sent_cb, pp_exp_cb, NULL);
    LeaveCriticalSection(&qcs);
}
static void pickpend_forget(void)
{
    EnterCriticalSection(&qcs);
    pp_forget(&g_pp);
    LeaveCriticalSection(&qcs);
}

static void world_frame(void)
{
    int active = net1_session_active();   /* before the model lock: it takes the net lock and the link lock */
    EnterCriticalSection(&mcs);
    g_status_active = active;
    int want = wh_want(&H, active);
    if (want && !last_want) hooks_apply();
    else if (!want && last_want) {
        if (bound) bound = 0, wlog("model unbound (hooks off)");
        hooks_revert();
    }
    last_want = want;
    if (want || bound) model_sync();
    if (bound) {
        if (cs_phase(bound_cs) == CP_PHASE_PLAYING) {   /* the level instance before the queue: events of the new level go out in its epoch */
            w1_set_epoch(&N.w, cs_epoch(bound_cs));
            story_epoch(cs_now(bound_cs));
            story_pre_drain();
        }
        drain();
        w1n_frame(&N);
        zone_pass(cs_phase(bound_cs) == CP_PHASE_PLAYING);   /* round 9: the puppet-authority zone rule (zone1_game.inc) */
        if (cs_phase(bound_cs) == CP_PHASE_PLAYING) {
            uint32_t now = cs_now(bound_cs);
            story_pass(now);
            prompt_pass(now);
            if (bound_role == W1_HOST) host_pass(now);
            local_pass(now);   /* rev1 live 11: the enemy log, the client's side of local1.h */
            hitx_pass(now);    /* rev1 live 14: the stock death seen / a dead enemy finished (hitx1.h rule 3) */
            motion_pass(now);  /* rev1 live 14: the own-AI enemies' motions to the other PC's replicas (mot1.h) */
            bag_pass(now);     /* rev1 live 14: the own human's guns to the other PC, the puppet follows (bag1.h) */
            watch_pass(now);   /* rev1 live 13: reverse replicas, the watcher's visible replicas and minimap (watch1.h) */
            door_pass(now);
            vital_pass(now);
            pickpend_pass();
            unsigned ep = w1_epoch(&N.w);
            if (ep != last_epoch_seen) {
                last_epoch_seen = ep;
                wh_keyset_clear(&credited);   /* kill credits and known door states belong to one level instance */
                ndoors = 0;
                pickpend_forget();
                local_reset();   /* rev1 live 11: the enemy log and the local1 table belong to one level instance */
                wlog("level instance (epoch %u): model reset%s", ep, bound_role == W1_HOST ? ", the host's pass registers the placed enemies" : ", the client waits for the host's SPAWN events");
            }
        }
    }
    if (last_want || bound || solo_w.state != S1_W_IDLE || solo_w.back_pending) solo_pass();   /* ep1-solo: the story's switched-off humans and the watch */
    hunk_local_pass(GetTickCount());   /* hunk1.h: the start gun and the fall-out rescue of a Hunk human, session or not */
    LeaveCriticalSection(&mcs);
}

int world_init(void)
{
    if (inited) return 0;
    inited = 1;
    InitializeCriticalSection(&mcs);
    InitializeCriticalSection(&qcs);
    char v[16];
    wh_init(&H, r1_env("REV1_WORLD", v, sizeof v) && atoi(v) == 1);
    lt_add_frame_cb(world_frame);
    sv_init();   /* ep1-probe: the save-flag probe (LAB, REV1_SVPROBE=1) */
    r1_log("world: module ready (REV1_WORLD=%d; the hooks are written only in a network session, with REV1_WORLD=1 (observe only) or by `world apply`)", H.env_on);
    return 0;
}

/* ---------------------------------------------------------------- console */
/* LAB (rev1 live 10): world kill <x> <z> <r> - the placed enemies alive within r of (x, z) die the way op_kill makes a replica die (Damage(hp), then OnHit
 * with an attacker-less HitInfo: no kill record). The lab profiles carry no weapon (l10b: the 13_048_0 zombies at the barn key bit the lab Jill to death);
 * this stands in for a player's shots. Meant for the HOST (its world is authoritative: the deaths go to the client as usual). At most 8 per call.
 * rev1 live 13: `world kill <x> <z> <r> by <slot>` builds the HitInfo with the human of <slot> as attacker (op_apply_hit's way for a client's hit): m13d
 * found that the attacker-less LAB kill of the 1-1 Ooze did not end 1-1 (the story's dead check reads the record's used-up byte). */
static void world_kill(const char *arg, char *out, int size)
{
    float kx = 0, kz = 0, kr = 0;
    uint32_t sa = sarrange(), att = 0;
    int o = 0, n = 0, by = -1;
    if (sscanf(arg, "%*s %f %f %f by %d", &kx, &kz, &kr, &by) < 3 || kr <= 0) { snprintf(out, (size_t)size, "usage: world kill <x> <z> <radius> [by <slot>]"); return; }
    if (by == 0 || by == 1) wh_player_by_slot(&MEM, (unsigned)by, &att);
    if (!sa || !call_ok(WH_C_DAMAGE) || H.state[WH_ONHIT] != WH_ON) { snprintf(out, (size_t)size, "world kill: no level / Damage or OnHit not ready"); return; }
    o += snprintf(out + o, (size_t)(size - o), "world kill (LAB) around (%.0f, %.0f) r %.0f%s:", (double)kx, (double)kz, (double)kr, att ? " with the attacker of the slot given" : "");
    unsigned cnt = wh_list_count(&MEM, sa, WH_LIST_ENEMIES);
    for (unsigned i = 0; i < cnt && n < 8 && o + 120 < size; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, WH_LIST_ENEMIES, i, &e) || !e.unit || !enemy_unit(e.unit)) continue;
        float x = rdf(e.unit + WH_U_POS), z = rdf(e.unit + WH_U_POS + 8), hp = rdf(e.unit + WH_U_HP);
        if (!(hp > 0.0f) || (x - kx) * (x - kx) + (z - kz) * (z - kz) > kr * kr) continue;
        uint8_t hit[WH_HIT_SIZE];
        float pt[3] = {x, rdf(e.unit + WH_U_POS + 4), z};
        int via = hitx_lab_kill(e.unit, att, by, pt);   /* rev1 live 14: through the hit queue, so the stock death follows (hitx1.h) */
        if (!via) {
            wh_hit_build(hit, WH_HIT_VTBL, att, att, 0, pt);
            ops_enter();
            g_allow_kill = 1;
            ((damage_fn)(uintptr_t)wh_sites[WH_C_DAMAGE].addr)((void *)(uintptr_t)e.unit, hp);
            ((hit_fn)(uintptr_t)wh_sites[WH_ONHIT].addr)((void *)(uintptr_t)e.unit, hit);
            g_allow_kill = 0;
            ops_leave();
        }
        n++;
        o += snprintf(out + o, (size_t)(size - o), " %08x (%.0f,%.0f) hp %.0f -> %.0f%s;", (unsigned)e.unit, (double)x, (double)z, (double)hp, (double)rdf(e.unit + WH_U_HP), via ? " (hit queue)" : " (Damage + OnHit)");
    }
    snprintf(out + o, (size_t)(size - o), " %d killed", n);
    wlog("%s", out);
}

static void world_list(char *out, int size)
{
    uint32_t sa = sarrange();
    int o = 0;
    if (!sa) {
        snprintf(out, (size_t)size, "world list: sArrange is not there (no level)");
        return;
    }
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_ENEMIES);
    o += snprintf(out + o, (size_t)(size - o), "world list: %u placed units (key = block*256+index; model state: 0 unknown 1 alive 2 dead 3 gone)", n);
    for (unsigned i = 0; i < n && o < size - 140; i++) {
        wh_entry e;
        unsigned b = 0, idx = 0;
        if (!wh_list_at(&MEM, sa, WH_LIST_ENEMIES, i, &e)) continue;
        int has = wh_entry_key(&MEM, sa, &e, &b, &idx);
        w1_key key = W1_KEY(b, idx);
        uint32_t ic = e.unit ? rd32(e.unit + 0xECC) : 0;   /* interpolator: its target queue +0x10 (sentinel) / +0x18 (first) */
        o += snprintf(out + o, (size_t)(size - o), "\n  %s%u/%u unit %08x class %08x kind %u E2C %u hp %.0f pos (%.0f,%.0f,%.0f) E1C %08x model %d st %d interp %s", has ? "" : "?", b, idx, (unsigned)e.unit, (unsigned)wh_unit_class(&MEM, e.unit),
                      rd32(e.unit + WH_U_KIND), rd32(e.unit + WH_U_SERIAL), rdf(e.unit + WH_U_HP), rdf(e.unit + WH_U_POS), rdf(e.unit + WH_U_POS + 4), rdf(e.unit + WH_U_POS + 8), rd32(e.unit + WH_U_FLAGS),
                      bound && has ? w1_enemy_state(&N.w, key) : -1, e.unit ? (int)interp_state(e.unit) : -1, !ic ? "none" : rd32(ic + 0x18) == ic + 0x10 ? "queue empty" : "queue USED");
    }
}

/* LAB (allep step 4, the Hunk study): `world plist` = the placed PLAYER units (sArrange+0x6C, class 0x8001xxxx): how the campaign makes the humans and what the Hunk class looks like. Read only. */
static void world_plist(char *out, int size)
{
    uint32_t sa = sarrange();
    int o = 0;
    if (!sa) {
        snprintf(out, (size_t)size, "world plist: sArrange is not there (no level)");
        return;
    }
    unsigned n = wh_list_count(&MEM, sa, 0x6Cu);
    o += snprintf(out + o, (size_t)(size - o), "world plist: %u placed player entries (sArrange+0x6C); class ids (DTI+0x1C): Hunk %08x Jill %08x Parker %08x ParkerLight %08x Chris %08x", n, rd32(0x117B630u), rd32(0x117B5F0u),
                  rd32(0x117B6D0u), rd32(0x117B710u), rd32(0x117B830u));
    for (unsigned i = 0; i < n && o < size - 200; i++) {
        wh_entry e;
        unsigned b = 0, idx = 0;
        uint32_t rc = 0;
        if (!wh_list_at(&MEM, sa, 0x6Cu, i, &e)) continue;
        int has = wh_entry_key(&MEM, sa, &e, &b, &idx);
        if (has) wh_char_class(&MEM, sa, b, idx, &rc);
        o += snprintf(out + o, (size_t)(size - o), "\n  entry %08x %s%u/%u unit %08x unit class %08x record class %08x E2C %u kind %u hp %.0f spent %u keys %08x %08x %08x %08x", (unsigned)e.entry, has ? "" : "?", b, idx, (unsigned)e.unit,
                      (unsigned)wh_unit_class(&MEM, e.unit), (unsigned)rc, e.unit ? rd32(e.unit + WH_U_SERIAL) : 0, e.unit ? rd32(e.unit + WH_U_KIND) : 0, e.unit ? rdf(e.unit + WH_U_HP) : 0.0f, rd8(e.entry + WH_ENTRY_SPENT),
                      (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2], (unsigned)e.k[3]);
    }
}

/* LAB (allep step 4, the Hunk study): `world pparam [id]` = the arrangement parameter object of a player slot (FUN_0044F410(sArrange, id): looked up by the +0xC id in the six lists, the one
 * uNpc_vf58 / uPlayer read for its class and tempParam): class name + the first 0xC0 bytes as dwords. Read only (the lookup is a pure list walk; it is called through its own address). */
typedef void *(TC *pparam_fn)(void *self, int id);
static void world_pparam(const char *a2, char *out, int size)
{
    uint32_t sa = sarrange();
    int o = 0;
    if (!sa) {
        snprintf(out, (size_t)size, "world pparam: sArrange is not there (no level)");
        return;
    }
    for (int id = a2[0] ? atoi(a2) : 0; id <= (a2[0] ? atoi(a2) : 1); id++) {
        uint32_t p = (uint32_t)(uintptr_t)((pparam_fn)(uintptr_t)0x44F410u)((void *)(uintptr_t)sa, id);
        char nm[96] = "";
        if (p) classes_object_name(p, nm, sizeof nm);
        o += snprintf(out + o, (size_t)(size - o), "%sid %d -> %08x %s", id ? "\n" : "", id, (unsigned)p, nm);
        for (unsigned i = 0; p && i < 0x30 && o < size - 120; i++)
            o += snprintf(out + o, (size_t)(size - o), "%s%s%08x", (i % 8) ? " " : "\n  +", (i % 8) ? "" : "", (unsigned)rd32(p + i * 4));
    }
}

/* LAB (allep step 4, the Hunk study): `world pcls [slot hex]` = the two player class ids the NEXT level start builds its humans from: [[0x116A340] + 0x24 + 4*slot] (cSystem_20, defaults Jill 0x80011010 / Parker 0x80011020;
 * FUN_0042B720 preloads their resources, FUN_0042A730 creates the units, a slot-1 id 0x80000000 = a one-character section). With arguments it WRITES the id (0x8001xxxx or 0x80000000 only):
 * `world pcls 1 800110b0` then an in-level restart makes slot 1 a Hunk (class 0x800110B0, resource pl10b0). Nothing happens until the next level start. Never used by the product. */
static DWORD WINAPI pcls_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {   /* LAB: every 3 ms, independent of any pass (a van run without a session has no world pass): the persisted ids are written back */
        uint32_t obj = rd32(0x116A340u);
        for (unsigned s = 0; s < 2; s++) {
            uint32_t v = pcls_persist[s];
            if (v && obj && rd32(obj + 0x24 + 4 * s) != v) {
                wr(obj + 0x24 + 4 * s, &v, 4);
                wlog("pcls persist: the player class of slot %u was set to %08x again (the engine had written something else)", s, (unsigned)v);
            }
        }
        Sleep(3);
    }
    return 0;
}

static volatile LONG pcls_started;

/* the persisted class id of a player slot (hunk1.h; the lab command `world pcls persist` too): id 0 = stop writing, slot 1 = Hunk for the solo part. Any thread. The writer thread starts
 * with the first non-zero id; it only writes when the engine's value differs, so a clear is just "id 0". */
void world_player_class(unsigned slot, uint32_t id)
{
    if (slot > 1) return;
    pcls_persist[slot] = id;
    if (id && !InterlockedExchange(&pcls_started, 1)) CreateThread(NULL, 0, pcls_thread, NULL, 0, NULL);
    wlog("pcls: the persisted player class of slot %u is %08x now (0 = off)", slot, (unsigned)id);
}

static void world_pcls(const char *arg, const char *a2, const char *a3, char *out, int size)
{
    uint32_t obj = rd32(0x116A340u);
    if (!obj) {
        snprintf(out, (size_t)size, "world pcls: the player setup object [0x116A340] is not there");
        return;
    }
    if (!strcmp(a2, "persist")) {   /* world pcls persist <slot> <hexid> | persist off */
        char a4[24] = "";
        unsigned slot = (unsigned)atoi(a3);
        sscanf(arg, "%*s %*s %*s %23s", a4);   /* arg = "pcls persist <slot> <hexid>" */
        if (!strcmp(a3, "off")) world_player_class(0, 0), world_player_class(1, 0);
        else if (slot < 2 && a4[0]) world_player_class(slot, (uint32_t)strtoul(a4, NULL, 16));
        snprintf(out, (size_t)size, "world pcls persist: slot 0 %08x, slot 1 %08x (0 = off); `world pcls` shows the object", (unsigned)pcls_persist[0], (unsigned)pcls_persist[1]);
        return;
    }
    if (a2[0] && a3[0]) {
        unsigned slot = (unsigned)atoi(a2);
        uint32_t v = (uint32_t)strtoul(a3, NULL, 16);
        if (slot > 1 || !(v == 0x80000000u || (v & 0xFFFF0000u) == 0x80010000u)) {
            snprintf(out, (size_t)size, "world pcls: slot 0 or 1 and an id 0x8001xxxx / 0x80000000 please");
            return;
        }
        wr(obj + 0x24 + 4 * slot, &v, 4);
        wlog("LAB pcls: player class of slot %u set to %08x (read back %08x)", slot, (unsigned)v, (unsigned)rd32(obj + 0x24 + 4 * slot));
    }
    snprintf(out, (size_t)size, "world pcls: object %08x: slot 0 class %08x, slot 1 class %08x (Jill 80011010, Parker 80011020, ParkerLight 80011420, Hunk 800110b0; slot 1 = 80000000 means a one-character section)", (unsigned)obj,
             (unsigned)rd32(obj + 0x24), (unsigned)rd32(obj + 0x28));
}

/* LAB ONLY (allep step 4, "hp 0 about 5 s after an unscripted placement"): `world hpbp [off]` = a hardware WRITE breakpoint (DR0 / DR1, 4 bytes) on the hp (unit+0xE38) of the humans of slot 0 / 1 on every thread; a vectored
 * handler logs the code address that wrote it (EIP after the store) with the stack words that look like code addresses, the old and the new value. Finds the writer of a hp drain that is not Damage(). Re-arm after every level
 * start (new units). Never used by the product. */
static volatile uint32_t hpbp_addr[2];
static volatile float hpbp_last[2];
static volatile LONG hpbp_hits, hpbp_installed;
static volatile int hpbp_keep;   /* `world hpbp keep`: a write that takes the hp to <= 0 is put back to the unit's max hp at once (lab god mode that also names the writer) */
static LONG CALLBACK hpbp_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 3u)) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 2; i++) {
        if (!(ep->ContextRecord->Dr6 & (1u << i)) || !hpbp_addr[i]) continue;
        float nv = rdf(hpbp_addr[i]);
        if (nv != hpbp_last[i] && InterlockedIncrement(&hpbp_hits) <= 400) {   /* only the writes that CHANGE the value (SetHp runs every frame with the same number) */
            uint32_t *sp = (uint32_t *)(uintptr_t)ep->ContextRecord->Esp;
            char st[420] = "";
            int o = 0, n = 0;
            for (int k = 0; k < 120 && n < 16 && o < 380; k++) {
                uint32_t w = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), sp + k, &w, 4, NULL)) break;
                if (w >= 0x401000u && w < 0x1200000u) o += snprintf(st + o, sizeof st - (size_t)o, " %08x", (unsigned)w), n++;
            }
            wlog("hpbp: the hp of slot %d (unit hp at %08x) was written: %.1f -> %.1f by code at %08x, thread %lu, code words on the stack:%s", i, (unsigned)hpbp_addr[i], hpbp_last[i], nv, (unsigned)ep->ContextRecord->Eip,
                 (unsigned long)GetCurrentThreadId(), st);
        }
        if (hpbp_keep && nv <= 0.0f) {
            uint32_t unit = hpbp_addr[i] - WH_U_HP;
            float mx = rdf(unit + 0xE40u);
            if (!(mx > 0.0f)) mx = 1000.0f;
            *(volatile float *)(uintptr_t)hpbp_addr[i] = mx;
            wlog("hpbp keep: the hp of slot %d was set back to %.0f (the writer above took it to %.1f)", i, mx, nv);
            nv = mx;
        }
        hpbp_last[i] = nv;
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#include <tlhelp32.h>
static int hpbp_arm(int on, char *out, int size)
{
    uint32_t unit[2] = {0, 0};
    for (unsigned s = 0; s < 2; s++) wh_player_by_slot(&MEM, s, &unit[s]);
    if (!hpbp_installed) hpbp_installed = AddVectoredExceptionHandler(1, hpbp_veh) != NULL;
    for (int i = 0; i < 2; i++) {
        hpbp_addr[i] = on && unit[i] ? unit[i] + WH_U_HP : 0;
        hpbp_last[i] = hpbp_addr[i] ? rdf(hpbp_addr[i]) : 0.0f;
    }
    int threads = 0, failed = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) { failed++; continue; }
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT c;
                memset(&c, 0, sizeof c);
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &c)) {
                    c.Dr0 = hpbp_addr[0], c.Dr1 = hpbp_addr[1], c.Dr6 = 0;
                    c.Dr7 = (hpbp_addr[0] ? 0xD0001u : 0u) | (hpbp_addr[1] ? 0xD00004u : 0u);
                    if (SetThreadContext(th, &c)) threads++; else failed++;
                } else failed++;
                ResumeThread(th);
            } else failed++;
            CloseHandle(th);
        }
        CloseHandle(snap);
    }
    /* this thread (the console / logic caller) too */
    {
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(GetCurrentThread(), &c)) {
            c.Dr0 = hpbp_addr[0], c.Dr1 = hpbp_addr[1], c.Dr6 = 0;
            c.Dr7 = (hpbp_addr[0] ? 0xD0001u : 0u) | (hpbp_addr[1] ? 0xD00004u : 0u);
            if (SetThreadContext(GetCurrentThread(), &c)) threads++;
        }
    }
    snprintf(out, (size_t)size, "world hpbp: %s; slot 0 hp at %08x (%.1f), slot 1 hp at %08x (%.1f); %d threads set, %d failed; hits so far %ld (logged: `hpbp:` lines)", on ? "ARMED" : "off", (unsigned)hpbp_addr[0],
             hpbp_last[0], (unsigned)hpbp_addr[1], hpbp_last[1], threads, failed, (long)hpbp_hits);
    return 1;
}

static void world_status(char *out, int size)
{
    size_t o = 0;
    wh_status(&H, &MEM, g_status_active, out, (size_t)size);
    o = strlen(out);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  model: %s%s%s; this PC's human slot %d, capture own slot %d; client's own enemy placements become replicas: %s; flags to the other PC: %s", bound ? (bound_role == W1_HOST ? "bound as HOST" : "bound as CLIENT") : "not bound",
                          g_core_why[0] ? " - " : "", g_core_why, (int)g_own, capture_own_effective(), g_replica ? "yes" : "no (world set replica 1)", f_story ? "story mirror ON (world sf)" : "story mirror off (world sf on)");
    o += (size_t)snprintf(out + o, (size_t)size - o,
                          "\n  hooks seen: SpawnChara %u (enemies %u, client replicas %u, reported %u, placed by the model %u, class mismatch %u); ApplyHit on enemies %u (client blows sent %u, host blows noted %u); replicas kept alive %u; deaths reported %u",
                          S.spawn_calls, S.spawn_enemy, S.spawn_replica, S.spawn_reported, S.spawn_ops, S.spawn_class_mismatch, S.hits_seen, S.hits_sent, S.hits_noted, S.replica_saved, S.deaths_reported);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  kill records %u (new %u); pickups sent %u (item not removed at once %u: sent later %u, still listed at the 60 s limit %u; own bag items not in the list %u); scenario flags seen %u (sent %u); queue overflow %u, dropped while loading %u / without a model %u; key out of range %u",
                          S.kill_records, S.kill_records_new, S.pickups_sent, S.pickups_not_removed, S.pickups_late, S.pickups_expired, S.pickups_unlisted, S.flags_seen, S.flags_sent, S.ev_overflow, S.ev_dropped_loading, S.ev_dropped_nomodel, S.key_overflow);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  placements: without a unit (block of another area) %u, nothing placed %u, host placements not announced (no unit) %u; replica kind fixed %u, setter refused %u",
                          S.spawn_nounit, S.spawn_none, S.report_nounit, S.kind_fixed, S.kind_refused);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  model callbacks: updates %u, kills %u, applied client hits %u, items taken %u (missing %u), flags %u, despawn switched off %u, ignored %u; host pass: spawns %u, deaths %u, despawns %u (kept by the fresh check %u)", S.ops_update,
                          S.ops_kill, S.ops_apply_hit, S.ops_item_gone, S.ops_item_missing, S.ops_flag, S.ops_despawn_stub, S.ops_noop, S.pass_spawn, S.pass_death, S.pass_despawn, S.pass_kept);
    if (bound) {
        char sum[600];
        w1_summary(&N.w, sum, sizeof sum);
        o += (size_t)snprintf(out + o, (size_t)size - o, "\n  %s", sum);
    }
    for (int g = 0; g < WH_N_GATES && o < (size_t)size; g++) {
        char live[96];
        wh_gate_probe(&MEM, g, live, sizeof live);
        o += (size_t)snprintf(out + o, (size_t)size - o, "\n  gate %-12s %08x: state %s, live bytes %s; applied %u, reverted %u, refused %u%s%s", wh_gates[g].name, wh_gates[g].addr + (unsigned)wh_gates[g].off, wh_state_name(G.state[g]), live,
                              G.applies[g], G.reverts[g], G.refusals[g], G.why[g][0] ? ", why: " : "", G.why[g]);
    }
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  switches: credit %d, despawn %d (0 off 1 vf15 2 fallout; sTigerTrap %08x), pchit %d, interp %d (yawidx %d), doorlog %d, doors %d (state %s), itemspent %d, vital %d, death87 %d, replica %d",
                          f_credit, f_despawn, rd32(0x116FFF4u), f_pchit, f_interp, f_yawidx, f_doorlog, f_doors, wh_door_states[f_doorstate].name, f_itemspent, f_vital, f_death87, g_replica);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  raid-reuse: credits %u (repeat refused %u, not written %u); despawns %u (fallout path %u, no entry %u, slot 15 refused %u), host parked entries seen %u; enemy blows on the partner sent %u (not from an enemy %u), applied here %u (refused %u)",
                          S.credit_done, S.credit_dup, S.credit_refused, S.despawn_done, S.despawn_fallout, S.despawn_noentry, S.despawn_vf_refused, S.pass_parked, S.pchit_sent, S.pchit_dropped, S.pchit_applied, S.pchit_refused);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  replica snaps %u (threshold %d, no move object %u), replica hp taken over %u (revive %d)", S.snaps, f_snap, S.snap_nomove, S.revived, f_revive);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  partner puppet blows dropped %u (puppethits %d), puppet kills not credited (not driven by the partner's pad) %u", S.puppet_dropped, f_puppethits, S.puppet_credit_refused);
    o += (size_t)snprintf(out + o, (size_t)size - o, "\n  interp targets %u (direct position writes %u, heading samples %u); doors:changes %u (logged %u), announced %u, set %u (refused %u, ignored %u), closes announced %u, closed here %u (left %u); items marked used up %u; puppet hp set %u; deaths from 0x87BC60 %u",
                          S.interp_targets, S.interp_direct, S.yaw_samples, S.door_changes, S.door_logged, S.door_announced, S.door_set, S.door_set_refused, S.door_ignored, S.door_close_announced, S.door_closed, S.door_close_left, S.item_spent, S.vital_set, S.death87);
    if (o + 700 < (size_t)size) {
        o += (size_t)snprintf(out + o, (size_t)size - o, "\n  ");
        solo_status(out + o, (int)((size_t)size - o));
        o += strlen(out + o);
    }
    if (o < (size_t)size) snprintf(out + o, (size_t)size - o, "\n  needs the guards (`guards status`): EXIT for kills with a knife (crash exe+1396), PICK for a pickup by the second human\n  not done (TODO): enemy loot, replica animation / sleep bit (state number), the despawn hook on the host (the list watcher does it)");
}

static int set_switch(const char *name, const char *v, char *out, int size)
{
    static const struct { const char *name; volatile int *var; int max; } T[] = {
        {"credit", &f_credit, 1}, {"spawnfh", &f_spawnfh, 1}, {"pchit", &f_pchit, 1}, {"interp", &f_interp, 1}, {"doorlog", &f_doorlog, 1}, {"doors", &f_doors, 1}, {"itemspent", &f_itemspent, 1},
        {"vital", &f_vital, 1},   {"death87", &f_death87, 1}, {"despawn", &f_despawn, 2}, {"yawidx", &f_yawidx, 2}, {"revive", &f_revive, 1}, {"puppethits", &f_puppethits, 1},
        {"story", &f_story, 1},   {"localrun", &f_localrun, 1}, {"rearr", &f_rearr, 1}, {"ewatch", &f_ewatch, 1}, {"watchzone", &f_watchzone, 1}, {"watchstage", &f_watchstage, 1},
        {"watchrep", &f_watchrep, 1}, {"rloc", &f_rloc, 1}, {"watchmap", &f_watchmap, 1}, {"zkeep", &f_zkeep, 1}, {"fallguard", &f_fallguard, 1}, {"fallres", &f_fallres, 1},
        {"hitx", &f_hitx, 1}, {"rdeath", &f_rdeath, 1}, {"deadguard", &f_deadguard, 1}, {"watchcam", &f_watchcam, 1}, {"watchlight", &f_watchlight, 1}, {"motion", &f_motion, 1}, {"bagsync", &f_bagsync, 1},
        {"bagdlc", &f_bagdlc, 1}, {"bagroom", &f_bagroom, 1},
    };
    if (!strcmp(name, "snap") && v[0] >= '0' && v[0] <= '9') {   /* distance in units, 0 = off */
        int d = atoi(v);
        f_snap = d < 0 ? 0 : d > 100000 ? 100000 : d;
        snprintf(out, (size_t)size, "world: snap = %d", f_snap);
        return 1;
    }
    if (!strcmp(name, "finish") && v[0] >= '0' && v[0] <= '9') {   /* rev1 live 14 (hitx1.h rule 3): ms after the model's death, 0 = off */
        int d = atoi(v);
        f_finish = d < 0 ? 0 : d > 600000 ? 600000 : d;
        snprintf(out, (size_t)size, "world: finish = %d ms", f_finish);
        return 1;
    }
    if (!strcmp(name, "doorstate") && (!strcmp(v, "auto") || !strcmp(v, "opened"))) {
        f_doorstate = !strcmp(v, "auto") ? WH_DS_OPENING_AUTO : WH_DS_OPENED;
        snprintf(out, (size_t)size, "world: a door the partner opened is set to %s here", wh_door_states[f_doorstate].name);
        return 1;
    }
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(name, T[i].name)) continue;
        if (v[0] < '0' || v[0] > '0' + T[i].max || v[1]) return 0;
        *T[i].var = v[0] - '0';
        if (T[i].var == &f_interp && last_want) {   /* the gates follow the switch at once */
            if (f_interp && call_ok(WH_C_TGT))
                for (int g = 0; g < WH_N_GATES; g++) wh_gate_apply(&G, &MEM, g);
            else if (!f_interp)
                gates_revert();
        }
        snprintf(out, (size_t)size, "world: %s = %d", T[i].name, *T[i].var);
        return 1;
    }
    return 0;
}

void world_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "", a2[16] = "", a3[16] = "";
    sscanf(arg, "%15s %15s %15s", sub, a2, a3);
    EnterCriticalSection(&mcs);
    if (!inited) {
        snprintf(out, (size_t)size, "world: not initialised");
    } else if (!strcmp(sub, "apply")) {
        H.mode = G1_MODE_ON;
        if (!last_want) hooks_apply();
        last_want = 1;
        world_status(out, size);
    } else if (!strcmp(sub, "revert")) {
        H.mode = G1_MODE_OFF;
        if (bound) bound = 0, wlog("model unbound (revert)");
        hooks_revert();
        last_want = 0;
        world_status(out, size);
    } else if (!strcmp(sub, "auto")) {
        H.mode = G1_MODE_AUTO;
        world_status(out, size);
    } else if (!sub[0] || !strcmp(sub, "status")) {
        world_status(out, size);
    } else if (!strcmp(sub, "list")) {
        world_list(out, size);
    } else if (!strcmp(sub, "plist")) {   /* LAB (Hunk study) */
        world_plist(out, size);
    } else if (!strcmp(sub, "pparam")) {   /* LAB (Hunk study) */
        world_pparam(a2, out, size);
    } else if (!strcmp(sub, "hpbp")) {   /* LAB (hp 0 study) */
        hpbp_keep = !strcmp(a2, "keep");
        hpbp_arm(strcmp(a2, "off") != 0, out, size);
    } else if (!strcmp(sub, "pcls")) {   /* LAB (Hunk study) */
        world_pcls(arg, a2, a3, out, size);
    } else if (!strcmp(sub, "kill")) {   /* LAB: world kill <x> <z> <r> */
        world_kill(arg, out, size);
    } else if (!strcmp(sub, "items")) {
        world_items(out, size, a2[0] ? (float)atof(a2) : 0.0f, a3);
    } else if (!strcmp(sub, "doors")) {
        world_doors(out, size, a2[0] ? (float)atof(a2) : 0.0f);
    } else if (!strcmp(sub, "door") && !strcmp(a2, "open") && a3[0]) {   /* LAB: world door open <unit hex> [side 1|2]: the game's own open request on THIS PC only */
        uint32_t unit = (uint32_t)strtoul(a3, NULL, 16);
        char a4[8] = "";
        sscanf(arg, "%*s %*s %*s %7s", a4);
        doors_resolve();
        uint32_t id = unit ? cur_state(unit) : 0;
        if (!unit || !(wh_unit_class(&MEM, unit) & 0x80000000u) || id != dsid[WH_DS_CLOSED] || !dsid[WH_DS_CLOSED])
            snprintf(out, (size_t)size, "world door open: unit %08x is not a closed uDoorBase (state %s)", (unsigned)unit, ds_name(id));
        else
            snprintf(out, (size_t)size, "world door open %08x: %s", (unsigned)unit, door_request_open(unit, (uint8_t)(a4[0] ? atoi(a4) : 0), "lab request") ? "requested (the door's own update opens it)" : "REFUSED (call not verified)");
    } else if (!strcmp(sub, "flaglog")) {   /* lab (r6): the capped SetScenarioFlag log lines count again from 0 (merge: also the story mirror's caps) */
        S.flags_seen = 0;
        sf_logged = sf_logged_low = sf_apply_logged = 0;
        snprintf(out, (size_t)size, "world: scenario flag log caps reset (the next 60 unmirrored-session / 200 not mirrored / 3000 story lines are logged)");
    } else if (!strcmp(sub, "zone")) {   /* round 9: world zone [status] | on | off (lab) | reset */
        zone_cmd(a2, arg, out, size);
    } else if (!strcmp(sub, "flags") && (!strcmp(a2, "on") || !strcmp(a2, "off"))) {   /* old name of `world sf on|off` */
        f_story = !strcmp(a2, "on");
        snprintf(out, (size_t)size, "world: story mirror (scenario flags, Genesis rate, set pieces) %s", f_story ? "ON" : "off");
    } else if (!strcmp(sub, "solo")) {   /* ep1-solo: world solo [status] | scope 0|1 | watch 0|1 */
        solo_cmd(a2, a3, out, size);
    } else if (!strcmp(sub, "pr") || !strcmp(sub, "prompt")) {   /* ep1-probe E8: world pr [status] | on | off (lab) */
        if (!strcmp(a2, "on") || !strcmp(a2, "off")) f_prompt = !strcmp(a2, "on");
        if (!strcmp(a2, "slots")) prompt_slots(out, size);
        else prompt_status(out, size);
    } else if (!strcmp(sub, "sf") || !strcmp(sub, "story")) {
        char a4[16] = "";
        sscanf(arg, "%*s %*s %*s %15s", a4);
        story_cmd(a2, a3, a4, out, size);
    } else if (!strcmp(sub, "local")) {   /* rev1 live 11: the enemies the host does not simulate (local1.h) */
        local_status(out, size);
    } else if (!strcmp(sub, "hitx")) {   /* rev1 live 14: the client's kills by the game's own death (hitx1.h) */
        hitx_status(out, size);
    } else if (!strcmp(sub, "motion")) {   /* rev1 live 14: the replicas' motions (mot1.h) */
        motion_status(out, size);
    } else if (!strcmp(sub, "bag")) {   /* rev1 live 14: the puppets' guns (bag1.h) */
        bag_status(out, size);
    } else if (!strcmp(sub, "watch")) {   /* rev1 live 13: reverse replicas, visible replicas, the minimap of the watching PC (watch1.h) */
        watch_status(out, size);
    } else if (!strcmp(sub, "set") && set_switch(a2, a3, out, size)) {
        /* answered */
    } else if (!strcmp(sub, "set") && (!strcmp(a2, "replica") || !strcmp(a2, "block")) && (a3[0] == '0' || a3[0] == '1')) {
        g_replica = a3[0] == '1';
        snprintf(out, (size_t)size, "world: the client's own enemy placements are %s", g_replica ? "made replicas (owner = host, kind 3)" : "left LOCAL (own AI; test only)");
    } else {
        snprintf(out, (size_t)size, "world status | apply (force on) | revert (force off) | auto (session / REV1_WORLD decides) | list (placed enemies with keys) | items [radius] [class] | doors [radius] | door open <unit> [side] (lab) | sf [status|on|off|wide 0|1|get <id>|set <id> <v>|rate [set <v>]] | set replica|credit|pchit|interp|doorlog|doors|itemspent|vital|death87|revive 0|1 | set despawn 0|1|2 | set doorstate auto|opened | set yawidx 0|1|2 | set snap <units, 0 off>");
    }
    LeaveCriticalSection(&mcs);
}
