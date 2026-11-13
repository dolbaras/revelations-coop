/*
 * bag1_game.inc - included by world1_game.c after motion1_game.inc: rev1 live round 14 (task 3), each puppet carries and holds its owner's real weapons (bag1.h).
 *   every 500 ms (logic thread, a level being played, a gameplay camera on viewport 0): this PC's own human's three weapon slots and current weapon go to
 *   the other PC (ext CP_W1_BAG) on a change and every 3 s; the received owner's bag is planned against this PC's puppet of him: the archive of a missing
 *   gun is loaded the stock way (sResource [0x11E00FC] +0x30 with DTI_rArchive), the gun added by PcsMainCharacterAddWeapon (FUN_00772500), the owner's
 *   current gun equipped by PcsMainNpcChangeEquipSlot (FUN_00775830); one action per 1.5 s. `world set bagsync 0|1`; `world bag` = counters.
 */
#include "bag1.h"

static volatile int f_bagsync = 1;
/* rev1 live 15 (m15a3 LIVE, r48): the host loaded the three DLC gun archives (dlc\archive\...wp1070 / 10a0 / 10b0, ~2.1 MB, kept for the session) and took the
 * puppet's M92F away; 12 ms after its 1-1 -> 1-2 section change began it died: `MessageBoxA "Fatal error.": Failed open file. ...
ativePC\model\chara\em * em1000\em1000.mod 3`, ExitProcess(1) (round 14 passed that change many times without either). Both stay LAB until a live run clears them (default 0):
 * `world set bagdlc 1` = load the DLC guns' archives for a puppet; `world set bagroom 1` = a full puppet bag gives up a gun (BG_REMOVE). With bagdlc 0 the
 * DLC guns are left out of the owner's list before the plan (the puppet keeps its own gun; nothing is taken away for a gun it cannot get). */
static volatile int f_bagdlc = 1, f_bagroom = 1;   /* fix021 (0.2.1): on by default through the stock path (LAB 0 = the 0.2.0 rules) */
static bg_msg bg_own_last, bg_peer;
static int bg_own_have, bg_peer_have;
static unsigned bg_peer_epoch;                   /* fix021 (bag1.h bg_peer_usable): the level instance the partner's last report arrived in */
static uint32_t bg_added[BG_ADDED_MAX];          /* fix021 (bag1.h bg_stale_mirror): the guns the mirror gave the puppet in this level instance */
static int bg_nadded;
static void bg_note_added(uint32_t c)
{
    for (int i = 0; i < bg_nadded; i++)
        if (bg_added[i] == c) return;
    if (bg_nadded < BG_ADDED_MAX) bg_added[bg_nadded++] = c;
}
static void bg_forget_added(uint32_t c)
{
    for (int i = 0; i < bg_nadded; i++)
        if (bg_added[i] == c) bg_added[i] = bg_added[--bg_nadded];
}
static int bg_seed_sent, bg_seed_have, bg_seed_applied, bg_armed_seen;   /* fix021: the host's seed of the guest's guns (bag1.h) */
static bg_msg bg_seed;
static int bg_kit_sent, bg_kit_have, bg_kit_applied, bg_knife_seen;   /* fix021 kit: the host's start kit of the guest's character (bag1.h, version 3) */
static bg_kit bg_kitv;
#define BG_PACK_AMMO_SET 0x5C2CE0u   /* fix021 kit: cBioItemPack vf11 (+0x2C) thiscall(pack; index, n): mBullet[index] = n (give1.c's "bag ammo set") */
static const uint8_t bg_ammo_sig[] = {0x8b, 0x44, 0x24, 0x04, 0x83, 0xf8, 0x0c, 0x73, 0x08, 0x8b, 0x54, 0x24};
typedef void(__attribute__((thiscall)) *bg_ammo_fn)(void *pack, unsigned index, unsigned n);
static uint32_t bg_level_ms;   /* fix021: the first gameplay pass of this level instance with this PC's human placed (bag1.h bg_gate) */
static int bg_unit_placed(uint32_t u)   /* fix021: the unit stands somewhere other than 0,0,0: the stock level start has placed it (story1's own_placed) */
{
    if (!u) return 0;
    float x = rdf(u + WH_U_POS), y = rdf(u + WH_U_POS + 4), z = rdf(u + WH_U_POS + 8);
    return isfinite(x) && isfinite(y) && isfinite(z) && fabsf(x) + fabsf(y) + fabsf(z) > 1.0f;
}
static uint32_t bg_own_ms, bg_last_pass, bg_apply_ms;
#define BG_ARCH_MAX 16
static uint32_t bg_arch[BG_ARCH_MAX], bg_arch_cls[BG_ARCH_MAX];   /* archives loaded for the puppet's guns (kept: a reference held for the session) */
static unsigned bg_narch;
static struct { unsigned tx, tx_fail, rx, bad, loads, load_fail, adds, add_fail, equips, refused, lines, removes, remove_fail; } BGS;
#define BG_SRES 0x11E00FCu
#define BG_DTI_ARCHIVE 0x11E0274u
#define BG_ADDWEAPON 0x772500u
#define BG_EQUIPSLOT 0x775830u
#define BG_PACK_VT 0x10186B0u      /* rev1 live 15: cBioItemPack */
#define BG_PACK_REMOVE 0x5C6AB0u   /* its vf7 (+0x1C): thiscall(pack; index): the item released (FUN_005D7D00), the slot cleared, the current index reset when held */
typedef void (TC *bg_remove_fn)(void *pack, unsigned index);
static const uint8_t bg_fsm_sig[] = {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x83, 0x7f, 0x08, 0x10, 0x8b, 0xf1, 0x75, 0x06, 0x8b, 0x74, 0x24};   /* both FSM actions (probe.c) */
typedef uint32_t (TC *bg_load_fn)(void *sres, void *dti, const char *path, int flags);
typedef unsigned (TC *bg_fsm4_fn)(void *self, void *param, void *x, void *unit);

static void bg_reset(void)
{
    bg_own_have = 0;   /* fix021: the own bag is sent again at the first gameplay pass of the new level (no archive is held by the mod) */
    bg_peer_have = bg_peer_have && bg_peer_epoch == w1_epoch(&N.w);   /* fix021: a report of THIS level instance (it may have come first in this frame) stays */
    bg_nadded = 0;
    bg_seed_sent = bg_seed_have = bg_seed_applied = bg_armed_seen = 0, bg_level_ms = 0;   /* fix021: one seed per level instance */
    bg_kit_sent = bg_kit_have = bg_kit_applied = bg_knife_seen = 0;   /* fix021 kit: one start kit per level instance */
}

/* the classes of a bag's words +4 .. +0x3C (0 = empty / not an item) and its current index; 1 = read */
static int bg_read(uint32_t unit, uint32_t cls[BG_SLOTS], int *cur)
{
    uint32_t p = unit ? rd32(unit + WH_U_BAG) : 0;
    if (!p) return 0;
    for (int i = 0; i < BG_SLOTS; i++) {
        uint32_t o = rd32(p + 4u + 4u * (unsigned)i);
        cls[i] = o > 0x10000 ? wh_unit_class(&MEM, o) : 0;
    }
    *cur = (int)rd32(p + 0xD4);
    return 1;
}

static int bg_fsm_ok(uint32_t fn)
{
    uint8_t cur[sizeof bg_fsm_sig];
    for (unsigned i = 0; i < sizeof cur; i++) cur[i] = (uint8_t)rd8(fn + i);
    return !memcmp(cur, bg_fsm_sig, sizeof cur);
}

/* m14e (LIVE): the stock load of a missing archive is FATAL ("Failed open file. ...\nativePC\archive\game\chara\wp\wp10b0\wp10b0.arc", the host's
 * ExitProcess(1)). rev1 live 15: that path was wrong for the DLC guns (bag1.h: "dlc\archive\..."); the check is now the stock one of the weapon loader
 * FUN_005CE180 -> FUN_008A2D60: FUN_009B12C0(sResource, DTI_rArchive, path) = the resource is found or its file exists (no load, no fatal error) */
#define BG_NOARCH_MAX 16
#define BG_RES_EXISTS 0x9B12C0u   /* thiscall(sResource; DTI, path) -> al, ret 8 */
static const uint8_t bg_exists_sig[] = {0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x9C, 0x24, 0x0C, 0x01, 0x00, 0x00};
typedef unsigned (TC *bg_exists_fn)(void *sres, void *dti, const char *path);
static uint32_t bg_noarch[BG_NOARCH_MAX];
static unsigned bg_nnoarch;
static int bg_arc_exists(uint32_t cls, const char *path)
{
    uint8_t cur[sizeof bg_exists_sig];
    for (unsigned i = 0; i < sizeof cur; i++) cur[i] = (uint8_t)rd8(BG_RES_EXISTS + i);
    uint32_t sres = rd32(BG_SRES);
    if (!sres || memcmp(cur, bg_exists_sig, sizeof cur)) {
        wlog("bag: the stock archive check FUN_009B12C0 differs / no sResource: gun %08x not loaded", (unsigned)cls);
        return 0;
    }
    ops_enter();
    unsigned r = ((bg_exists_fn)(uintptr_t)BG_RES_EXISTS)((void *)(uintptr_t)sres, (void *)(uintptr_t)BG_DTI_ARCHIVE, path) & 0xFFu;
    ops_leave();
    return r != 0;
}
/* fix021 (0.2.1, bag1.h): 1 = the stock weapon loader will find this class's archive (FUN_009B12C0, the check of FUN_005CE180 -> FUN_008A2D60). The mod loads
 * NOTHING itself any more (r48 held its own references for the whole session): the stock AddWeapon loads, refcounts and releases the archive. */
#define BG_OKARCH_MAX 32
static uint32_t bg_okarch[BG_OKARCH_MAX];
static unsigned bg_nokarch;
static int bg_arc_ok(uint32_t cls)
{
    for (unsigned i = 0; i < bg_nokarch; i++)
        if (bg_okarch[i] == cls) return 1;
    for (unsigned i = 0; i < bg_nnoarch; i++)
        if (bg_noarch[i] == cls) return 0;
    char path[96];
    if (!bg_arc_path(cls, path, sizeof path)) return 0;   /* the stock path of FUN_008A29A0 ("dlc\archive\..." for the DLC guns) */
    if (!bg_arc_exists(cls, path)) {
        if (bg_nnoarch < BG_NOARCH_MAX) bg_noarch[bg_nnoarch++] = cls;
        BGS.load_fail++;
        wlog("bag: gun %08x: the archive %s is not there for this game (the stock check FUN_009B12C0 said no)", (unsigned)cls, path);
        return 0;
    }
    if (bg_nokarch < BG_OKARCH_MAX) bg_okarch[bg_nokarch++] = cls;
    return 1;
}
/* fix021: the item manager's 16 archive slots ([0x116AB48] +0x2F8): a class it does not hold needs a free one (a full table leaks the archive) */
#define BG_ITEMMGR_PTR 0x116AB48u
static int bg_item_room(uint32_t cls)
{
    uint32_t im = rd32(BG_ITEMMGR_PTR), tab[16];
    if (!im || !r1_safe_read((const void *)(uintptr_t)(im + 0x2F8), tab, sizeof tab)) return 0;
    return bg_item_table_ok(tab, cls);
}
/* fix021: the owner's message as this PC can show it: a gun whose archive is not here becomes the nearest stock gun of its family (bag1.h bg_fallback) */
static void bg_mirrorable(bg_msg *m)
{
    for (int k = 0; k < 4; k++) {
        uint32_t *c = k < 3 ? &m->cls[k] : &m->cur_cls;
        if (!*c || !bg_is_gun(*c)) continue;
        if (!f_bagdlc && bg_is_dlc(*c)) {   /* LAB `world set bagdlc 0`: the 0.2.0 filter */
            *c = 0;
            continue;
        }
        uint32_t f = bg_fallback(*c, bg_arc_ok(*c));
        if (f != *c) {
            static uint32_t said[8];
            int known = 0;
            for (int i = 0; i < 8; i++) known |= said[i] == *c;
            for (int i = 0; i < 8 && !known; i++)
                if (!said[i]) {
                    said[i] = *c, known = 1;
                    wlog("bag: the partner's gun %08x has no files on this PC: his puppet shows the nearest stock gun %08x instead", (unsigned)*c, (unsigned)f);
                }
        }
        *c = f;
    }
}

static void bg_fsm_slot(uint32_t fn, uint32_t unit, uint32_t a, uint32_t slot)
{
    uint32_t param[8] __attribute__((aligned(4)));
    memset(param, 0, sizeof param);
    param[2] = 0x10, param[3] = a, param[4] = slot, param[5] = 1;   /* the unit is given; class / slot; the bag's free slot; one */
    ops_enter();
    ((bg_fsm4_fn)(uintptr_t)fn)(NULL, param, NULL, (void *)(uintptr_t)unit);
    ops_leave();
}

static void bg_fsm(uint32_t fn, uint32_t unit, uint32_t a) { bg_fsm_slot(fn, unit, a, 0xFFFFFFFFu); }

/* hunk1.h (the product's weapons for a Hunk human): Hunk has no profile bag (his bag holds only uWpUnarmed). A Hunk unit that is THIS PC's human (kind 1) and carries no gun is given the start
 * gun the stock way (the archive first, then the stock AddWeapon and ChangeEquipSlot FSM actions on the unit: the proven puppet route above, a weapon object of its own, nothing shared between
 * bags, so a reload / level teardown frees it like any other gun; packcopy's shared objects are NOT used). The puppet of a Hunk on the other PC follows by the bag sync. At most 6 tries per unit. */
static uint32_t hk_bag_unit, hk_bag_ms;
static unsigned hk_bag_tries;
static void hunk_bag_pass(uint32_t me, uint32_t now)
{
    if (!me || rd32(me + WH_U_KIND) != 1 || wh_unit_class(&MEM, me) != HK_CLASS_HUNK) return;
    if (!bg_unit_placed(me)) return;   /* fix021: not before the stock level start has placed him (bag1.h bg_gate) */
    if (me != hk_bag_unit) hk_bag_unit = me, hk_bag_tries = 0, hk_bag_ms = 0;
    uint32_t b[BG_SLOTS];
    int c = -1;
    if (!bg_read(me, b, &c) || bg_find(b, HK_START_GUN) >= 0 || b[0] || b[1] || b[2]) return;   /* has a gun (own or given) */
    if (hk_bag_tries >= 6 || (int32_t)(now - hk_bag_ms) < 1500) return;
    hk_bag_ms = now, hk_bag_tries++;
    if (!bg_fsm_ok(BG_ADDWEAPON) || !bg_fsm_ok(BG_EQUIPSLOT)) {
        wlog("hunk: the stock AddWeapon / ChangeEquipSlot bytes differ: Hunk gets no gun");
        return;
    }
    if (!bg_arc_ok(HK_START_GUN) || !bg_item_room(HK_START_GUN)) {
        wlog("hunk: the archive of the start gun %08x is not there / the item table is full: no gun", (unsigned)HK_START_GUN);
        return;
    }
    bg_fsm(BG_ADDWEAPON, me, HK_START_GUN);
    uint32_t after[BG_SLOTS];
    int ac = -1;
    bg_read(me, after, &ac);
    int idx = bg_find(after, HK_START_GUN);
    if (idx >= 0) bg_fsm(BG_EQUIPSLOT, me, (uint32_t)idx);
    wlog("hunk: start gun %08x for Hunk's human (unit %08x, try %u): %s (bag index %d)", (unsigned)HK_START_GUN, (unsigned)me, hk_bag_tries, idx >= 0 ? "added and equipped" : "NOT added", idx);
}

/* hunk1.h, every frame while the Hunk option is in force (the persisted class of slot 1 is Hunk's), in a network session AND in a local split game (no model needed): every Hunk human of THIS PC
 * (kind 1: the own human of the guest, or the second pad's human of a split game) gets (1) the start gun if he has none, (2) the fall-out rescue of fallres1.h: the solo part 16-2 has no start record for
 * slot 1, so he appears where there is no floor; without ground for 1.5 s he is put next to the partner before the engine's 5 s fall-out kills him. The puppet of a Hunk on the other PC does neither. */
/* The shot of a Hunk human crashed the guest (allep hk1, 18:54:57, exe+2B6B27): the upper-body code asks the unit's motion parameter table (FUN_004A3430: byte table [unit+0x2B8] indexed by a parameter id,
 * records of 0xA0 bytes from [unit+0x2B4]; 0xFF = no record -> NULL) for id [0x1136E44] and reads a float at +0x44 of the result. Hunk's table (a Raid-mode character: his motion list has not every campaign
 * parameter) has 0xFF there. Every missing id of a Hunk unit is pointed at the unit's first valid record, once per unit: plausible values instead of a null read (the table is logged). */
static uint32_t hk_mt_unit[2];
static void hunk_motion_fix(unsigned s, uint32_t u)
{
    if (hk_mt_unit[s] == u) return;
    hk_mt_unit[s] = u;
    uint32_t tbl = rd32(u + 0x2B8), rec = rd32(u + 0x2B4);
    if (tbl < 0x10000 || rec < 0x10000) {
        wlog("hunk: unit %08x: the motion parameter table [+0x2B8] = %08x, records [+0x2B4] = %08x are not usable: not fixed", (unsigned)u, (unsigned)tbl, (unsigned)rec);
        return;
    }
    int first = -1;
    unsigned valid = 0, missing = 0;
    char line[300];
    int o = 0;
    for (unsigned i = 0; i < 256; i++) {
        unsigned b = rd8(tbl + i);
        if (b != 0xFF) {
            valid++;
            if (first < 0) first = (int)b;
            if (o < (int)sizeof line - 12) o += snprintf(line + o, sizeof line - (size_t)o, "%u:%u ", i, b);
        } else missing++;
    }
    wlog("hunk: unit %08x: motion table %08x records %08x: %u ids valid (id:record) %s| %u missing (0xFF)", (unsigned)u, (unsigned)tbl, (unsigned)rec, valid, line, missing);
    if (first < 0) return;
    unsigned fixed = 0;
    for (unsigned i = 0; i < 256; i++)
        if (rd8(tbl + i) == 0xFF) wr8(tbl + i, (uint8_t)first), fixed++;
    wlog("hunk: unit %08x: %u missing motion parameter ids now point at record %d (no null record for the upper-body code)", (unsigned)u, fixed, first);
}

static frs_t FRSH[2];
/* lab T3 2026-11-12 + REA: 2-2's script (22_04_event_s103_ep, nodes LockParker / P_lock) runs FsmLockPlayer mIsLock=2 mIsLockPartner=1. Its exec FUN_0074FBD0 takes "the partner"
 * FUN_0041BC10 (the human that is NOT this PC's own one) and calls FUN_006B3B80 on it: unit+0xE1C |= 4, unit+0xF44 |= 8, its lower state to 0 (Upper::FullMoveWaitState, no
 * walk, no aim). In a network game the guest's Hunk is that PC's own human, the lock lands on the host's puppet there; in an offline split game the second human IS the partner
 * and stood frozen. The split's second human (Hunk) gets the stock unlock of the same action, FUN_006AE390 (mIsLockPartner=0: clears the two bits and puts the lower state
 * back), whenever the lock is on during play. */
#define HK_UNLOCK_FN 0x6AE390u
static void hunk_split_unlock(unsigned s, uint32_t u, uint32_t now)
{
    static const uint8_t pro[10] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x9E, 0x1C, 0x0E, 0x00, 0x00};
    static int ok = -1;
    static uint32_t last;
    static unsigned n;
    if (net1_session_active() || (int)s == capture_own_effective() || !(rd32(u + 0xE1C) & 4u)) return;
    if ((int32_t)(now - last) < 500) return;
    last = now;
    if (ok < 0) {
        uint8_t cur[10];
        ok = r1_safe_read((const void *)(uintptr_t)HK_UNLOCK_FN, cur, sizeof cur) && !memcmp(cur, pro, sizeof cur);
        if (!ok) wlog("hunk: FUN_006AE390 differs from the supported build: the split's second human stays locked as the story's partner");
    }
    if (!ok) return;
    uint32_t e0 = rd32(u + 0xE1C), f0 = rd32(u + 0xF44);
    ((uint32_t (__attribute__((thiscall)) *)(void *))(uintptr_t)HK_UNLOCK_FN)((void *)(uintptr_t)u);
    if (++n <= 8) wlog("hunk: the split's second human (Hunk, slot %u, unit %08x) was locked as the story's partner (E1C %08x F44 %08x): the stock unlock -> E1C %08x F44 %08x (#%u)", s, (unsigned)u,
                       (unsigned)e0, (unsigned)f0, (unsigned)rd32(u + 0xE1C), (unsigned)rd32(u + 0xF44), n);
}

static void hunk_local_pass(uint32_t now)
{
    if (pcls_persist[HK_SLOT] != HK_CLASS_HUNK) return;
    uint32_t root = rd32(SOLO_CAMROOT), vp0 = root ? rd32(root + 0x34) : 0, m0 = root ? rd32(root + 0xCE0) : 0, m1 = root ? rd32(root + 0xCE4) : 0;
    if (!vp0 || (vp0 != m0 && vp0 != m1)) return;   /* a cutscene / loading: nothing now (as bag_pass) */
    for (unsigned s = 0; s < 2; s++) {
        uint32_t u = 0, pu = 0;
        if (!wh_player_by_slot(&MEM, s, &u) || !u || wh_unit_class(&MEM, u) != HK_CLASS_HUNK) continue;
        hunk_motion_fix(s, u);   /* both PCs, own human or puppet */
        if (rd32(u + WH_U_KIND) != 1) continue;   /* the puppet of a Hunk on the other PC gets neither gun nor rescue */
        hunk_split_unlock(s, u, now);
        hunk_bag_pass(u, now);
        wh_player_by_slot(&MEM, 1 - s, &pu);
        uint32_t mo = rd32(u + 0xE88);
        int ng = mo && rd8(mo + 0x2B5) == 1 && rd8(mo + 8) == 1;
        if (frs_step(&FRSH[s], ng, rdf(u + WH_U_POS + 4), pu != 0, now ? now : 1)) {
            float pp[3] = {rdf(pu + WH_U_POS), rdf(pu + WH_U_POS + 4), rdf(pu + WH_U_POS + 8)}, to[3];
            uint32_t zero = 0;
            frs_target(pp, to);
            wr8(mo + 0x2B4, 1);   /* the move object's teleport byte (as op_update) */
            wr(u + WH_U_POS, to, 12);
            wr(u + 0x4C, &zero, 4);
            wrf(mo + 0x2B8, 0.0f);
            wlog("hunk: Hunk's human (slot %u, unit %08x) had no ground for %u ms: put next to the partner at (%.0f,%.0f,%.0f) before the engine's fall-out kills him (rescue #%u)", s, (unsigned)u, FRS_AFTER_MS, to[0], to[1], to[2], FRSH[s].rescues);
        }
    }
}

/* both PCs, logic thread, model lock, a level being played */
static void bag_pass(uint32_t now)
{
    if ((int32_t)(now - bg_last_pass) < 500) return;
    bg_last_pass = now;
    int own = g_own;
    if (!f_bagsync || (own != 0 && own != 1)) return;
    uint32_t root = rd32(SOLO_CAMROOT), vp0 = root ? rd32(root + 0x34) : 0, m0 = root ? rd32(root + 0xCE0) : 0, m1 = root ? rd32(root + 0xCE4) : 0;
    if (!vp0 || (vp0 != m0 && vp0 != m1)) return;   /* a cutscene / loading: nothing now */
    uint32_t me = 0, pup = 0;
    wh_player_by_slot(&MEM, (unsigned)own, &me);
    wh_player_by_slot(&MEM, (unsigned)(1 - own), &pup);
    uint32_t cls[BG_SLOTS];
    int cur = -1;
    if (!bg_gate(1, bg_unit_placed(me))) return;   /* fix021 (fw1 de2598fc 23:04): nothing before the stock level start has placed this PC's human */
    if (!bg_level_ms) bg_level_ms = now ? now : 1;
    if (!g_client && pup && !bg_seed_sent) {   /* fix021: the host's seed: the guns its save gave the guest's character, before any mirror change */
        uint32_t pc0[BG_SLOTS];
        int pcur0 = -1;
        if (bg_read(pup, pc0, &pcur0)) {
            bg_msg sm;
            memset(&sm, 0, sizeof sm);
            for (int k = 0; k < 3; k++) sm.cls[k] = bg_is_gun(pc0[k]) ? pc0[k] : 0;
            sm.cur = (uint8_t)(pcur0 >= 0 && pcur0 < 3 ? pcur0 : 0xFF), sm.cur_cls = pcur0 >= 0 && pcur0 < 3 ? sm.cls[pcur0] : 0;
            uint8_t b[BG_LEN];
            bg_enc_seed(b, &sm);
            if (w1_send_ext(&N.w, CP_W1_BAG, b, BG_LEN)) {
                bg_seed_sent = 1;
                wlog("bag: this host's save gave the guest's character %08x %08x %08x: sent once (the guest takes them only if it has no gun)", (unsigned)sm.cls[0], (unsigned)sm.cls[1], (unsigned)sm.cls[2]);
            }
        }
    }
    if (!g_client && pup && !bg_kit_sent) {   /* fix021 kit: the start kit the host's level start gave the guest's character (its knife, reserve ammo, herbs) */
        uint32_t pc0[BG_SLOTS], pk = rd32(pup + WH_U_BAG);
        int pcur0 = -1;
        if (pk && bg_read(pup, pc0, &pcur0)) {
            bg_kit k;
            memset(&k, 0, sizeof k);
            k.knife = bg_is_knife(pc0[BK_KNIFE_ITEM]) ? pc0[BK_KNIFE_ITEM] : 0;
            for (int i = 0; i < BK_AMMO; i++) {
                uint32_t v = rd32(pk + 0x40 + 4u * (unsigned)i);
                k.ammo[i] = (uint16_t)(v <= 999u ? v : 0u);
            }
            uint32_t h = rd32(pk + 0xC8);
            k.herb = (uint16_t)(h <= 99u ? h : 0u);
            uint8_t b[BK_LEN];
            bg_enc_kit(b, &k);
            if (w1_send_ext(&N.w, CP_W1_BAG, b, BK_LEN)) {
                bg_kit_sent = 1;
                wlog("bag: this host's level start gave the guest's character the knife %08x, ammo %u %u %u %u, herbs %u: sent once (the guest takes them only if its human has no knife)",
                     (unsigned)k.knife, (unsigned)k.ammo[0], (unsigned)k.ammo[1], (unsigned)k.ammo[2], (unsigned)k.ammo[3], (unsigned)k.herb);
            }
        }
    }
    if (me && rd32(me + WH_U_KIND) == 1 && bg_read(me, cls, &cur)) {   /* the own human's bag to the other PC */
        if (g_client && bg_has_gun(cls)) bg_armed_seen = 1;   /* fix021: a gun of its own (dealt, picked up, given): the seed never applies in this level */
        if (bg_seed_wanted(g_client, bg_seed_have, bg_seed_applied, bg_has_gun(cls), bg_armed_seen, bg_settled(bg_level_ms, now))) {
            bg_seed_applied = 1;   /* once, whatever the adds give */
            int added = 0;
            for (int k = 0; k < 3 && bg_fsm_ok(BG_ADDWEAPON); k++) {
                uint32_t c = bg_seed.cls[k];
                if (!bg_is_gun(c)) continue;
                c = bg_fallback(c, bg_arc_ok(c));
                uint32_t nowb[BG_SLOTS];
                int nc = -1;
                if (!c || !bg_read(me, nowb, &nc) || bg_find(nowb, c) >= 0 || !bg_item_room(c)) continue;
                bg_fsm(BG_ADDWEAPON, me, c), added++;
            }
            if (added && bg_fsm_ok(BG_EQUIPSLOT)) bg_fsm(BG_EQUIPSLOT, me, 0);
            bg_read(me, cls, &cur);
            wlog("bag: this PC's human had NO gun at the level start: the guns the host's save gave its character added (%d of %08x %08x %08x, the stock AddWeapon): now %08x %08x %08x",
                 added, (unsigned)bg_seed.cls[0], (unsigned)bg_seed.cls[1], (unsigned)bg_seed.cls[2], (unsigned)cls[0], (unsigned)cls[1], (unsigned)cls[2]);
        }
        if (g_client && bg_is_knife(cls[BK_KNIFE_ITEM])) bg_knife_seen = 1;   /* fix021 kit: a knife of its own in this level: the kit never applies */
        if (bg_kit_wanted(g_client, bg_kit_have, bg_kit_applied, bg_is_knife(bg_kitv.knife), bg_is_knife(cls[BK_KNIFE_ITEM]), bg_knife_seen,
                          bg_settled(bg_level_ms, now))) {
            bg_kit_applied = 1;   /* once, whatever the calls give */
            uint32_t pk = rd32(me + WH_U_BAG), vt = pk ? rd32(pk) : 0, af = vt == BG_PACK_VT ? rd32(vt + 4u * 11u) : 0;
            int oa[BK_AMMO], oh = pk ? (int)rd32(pk + 0xC8) : 1, knife = 0, sets = 0, ammo_ok = af == BG_PACK_AMMO_SET;
            for (int i = 0; i < BK_AMMO; i++) oa[i] = pk ? (int)rd32(pk + 0x40 + 4u * (unsigned)i) : 1;
            for (unsigned i = 0; ammo_ok && i < sizeof bg_ammo_sig; i++) ammo_ok = rd8(af + i) == bg_ammo_sig[i];
            bg_kit_plan p = bg_kit_plan_of(&bg_kitv, cls, oa, oh);
            if (p.knife && bg_fsm_ok(BG_ADDWEAPON) && bg_arc_ok(p.knife) && bg_item_room(p.knife)) bg_fsm_slot(BG_ADDWEAPON, me, p.knife, BK_KNIFE_ITEM), knife = 1;
            for (int i = 0; i < BK_AMMO && ammo_ok; i++)
                if (p.ammo[i] >= 0) ((bg_ammo_fn)(uintptr_t)af)((void *)(uintptr_t)pk, (unsigned)i, (unsigned)p.ammo[i]), sets++;
            if (pk && p.herb >= 0) wr32(pk + 0xC8, (uint32_t)p.herb), sets++;
            bg_read(me, cls, &cur);
            wlog("bag: this PC's human had NO knife at the level start (a bag the stock start never filled): the host's start kit: knife %08x %s (item 10 now %08x), %d count(s) set (ammo %s; herbs %d -> %d)",
                 (unsigned)p.knife, knife ? "added (the stock AddWeapon, slot 10)" : "not added", (unsigned)cls[BK_KNIFE_ITEM], sets, ammo_ok ? "stock vf11" : "NOT set: the pack's vf11 differs",
                 oh, pk ? (int)rd32(pk + 0xC8) : -1);
        }
        bg_msg m;
        memset(&m, 0, sizeof m);
        m.cur = (uint8_t)(cur >= 0 && cur < 0xFF ? cur : 0xFF);
        for (int k = 0; k < 3; k++) m.cls[k] = cls[k];
        m.cur_cls = cur >= 0 && cur < BG_SLOTS ? cls[cur] : 0;
        if (!bg_own_have || !bg_same(&m, &bg_own_last) || (uint32_t)(now - bg_own_ms) >= BG_REFRESH_MS) {
            uint8_t b[BG_LEN];
            bg_enc(b, &m);
            if (w1_send_ext(&N.w, CP_W1_BAG, b, BG_LEN)) {
                if (!bg_own_have || !bg_same(&m, &bg_own_last))
                    if (++BGS.lines <= 60) wlog("bag: this PC's human carries %08x %08x %08x, current %08x (index %d): sent to the other PC", (unsigned)m.cls[0], (unsigned)m.cls[1], (unsigned)m.cls[2], (unsigned)m.cur_cls, cur);
                bg_own_last = m, bg_own_have = 1, bg_own_ms = now, BGS.tx++;
            } else BGS.tx_fail++;
        }
    }
    /* fix021 (f21r1 04:13: bag_pass ran before the new level instance's reset and gave the 1-2 puppet the guest's 1-1 gun): only a report of this level instance */
    if (!bg_peer_usable(bg_peer_have, bg_peer_epoch, w1_epoch(&N.w)) || !pup || (int32_t)(now - bg_apply_ms) < (int32_t)BG_APPLY_GAP_MS) return;
    bg_msg want = bg_peer;   /* fix021: the owner's guns as this PC can show them (bag1.h bg_fallback; LAB bagdlc 0 = no DLC gun) */
    bg_mirrorable(&want);
    uint32_t pc[BG_SLOTS];
    int pcur = -1;
    if (!bg_read(pup, pc, &pcur)) return;
    int free_slot = !pc[0] || !pc[1] || !pc[2];
    bg_step s = bg_plan2(&want, pc, pcur, free_slot);   /* fix021: a full bag makes room by the stock replace (BG_REPLACE) */
    if (s.act == BG_REPLACE && !f_bagroom) return;   /* (LAB bagroom 0) */
    if (s.act == BG_NONE) {   /* fix021: a gun the mirror added in this level instance that he no longer lists goes, the stock way (bag1.h bg_stale_mirror) */
        int k = bg_stale_mirror(&want, pc, pcur, bg_added, bg_nadded);
        if (k < 0) return;
        uint32_t pk = rd32(pup + WH_U_BAG), pvt = pk ? rd32(pk) : 0, rm = pvt ? rd32(pvt + 0x1C) : 0, old = pc[k];
        bg_apply_ms = now;
        bg_forget_added(old);   /* one try per gun */
        if (pvt != BG_PACK_VT || rm != BG_PACK_REMOVE) {
            BGS.remove_fail++;
            if (++BGS.refused <= 5) wlog("bag: the puppet's bag (vtable %08x, slot 7 %08x) is not the stock cBioItemPack: its stale gun %08x stays", (unsigned)pvt, (unsigned)rm, (unsigned)old);
            return;
        }
        ops_enter();
        ((bg_remove_fn)(uintptr_t)rm)((void *)(uintptr_t)pk, (unsigned)k);
        ops_leave();
        uint32_t after[BG_SLOTS];
        int ac = -1;
        bg_read(pup, after, &ac);
        int ok = after[k] == 0;
        if (ok) BGS.removes++;
        else BGS.remove_fail++;
        if (++BGS.lines <= 60) wlog("bag: the partner's puppet (unit %08x) carried %08x (index %d) from an earlier report of his that he no longer lists: %s", (unsigned)pup, (unsigned)old, k,
                                    ok ? "taken away the stock way (cBioItemPack_vf7)" : "NOT taken away");
        return;
    }
    bg_apply_ms = now;
    if (s.act == BG_ADD) {
        if (!bg_fsm_ok(BG_ADDWEAPON)) {
            if (++BGS.refused <= 5) wlog("bag: PcsMainCharacterAddWeapon's bytes differ: the puppet gets nothing");
            return;
        }
        if (!bg_item_room(s.cls)) {
            if (++BGS.refused <= 5) wlog("bag: the item manager's 16 archive slots are full: the puppet does not get %08x now", (unsigned)s.cls);
            return;
        }
        bg_fsm(BG_ADDWEAPON, pup, s.cls);   /* the stock AddWeapon loads the archive itself (FUN_005CE180) */
        uint32_t after[BG_SLOTS];
        int ac = -1;
        bg_read(pup, after, &ac);
        int ok = bg_find(after, s.cls) >= 0;
        if (ok) BGS.adds++, bg_note_added(s.cls);
        else BGS.add_fail++;
        if (++BGS.lines <= 60) wlog("bag: the partner's puppet (unit %08x) %s his gun %08x (his current %08x): %s", (unsigned)pup, ok ? "now carries" : "did NOT take", (unsigned)s.cls,
                                    (unsigned)bg_peer.cur_cls, ok ? "added by the stock AddWeapon (its archive by the stock loader)" : "the add left the bag unchanged");
    } else if (s.act == BG_REPLACE) {   /* fix021: the stock replace: AddWeapon into that slot (the bag's FUN_005C7060 releases the old item, never the held one) */
        if (!bg_fsm_ok(BG_ADDWEAPON) || s.index < 0 || s.index > 2 || s.index == pcur || !bg_item_room(s.cls)) {
            BGS.remove_fail++;
            if (++BGS.refused <= 5) wlog("bag: no replace of the puppet's slot %d (held %d / the item table / the bytes): nothing changed", s.index, pcur);
            return;
        }
        uint32_t old = pc[s.index];
        bg_fsm_slot(BG_ADDWEAPON, pup, s.cls, (uint32_t)s.index);
        uint32_t after[BG_SLOTS];
        int ac = -1;
        bg_read(pup, after, &ac);
        int ok = after[s.index] == s.cls;
        if (ok) BGS.removes++, BGS.adds++, bg_note_added(s.cls);
        else BGS.remove_fail++;
        if (++BGS.lines <= 60) wlog("bag: the partner's puppet (unit %08x) had a full bag: its gun %08x (index %d, not his) %s his %08x (the stock replace)", (unsigned)pup, (unsigned)old,
                                    s.index, ok ? "replaced by" : "NOT replaced by", (unsigned)s.cls);
    } else if (s.act == BG_REMOVE) {   /* rev1 live 15: a full puppet bag makes room: one gun the owner does not carry, never the held one (bag1.h) */
        uint32_t pk = rd32(pup + WH_U_BAG), pvt = pk ? rd32(pk) : 0, rm = pvt ? rd32(pvt + 0x1C) : 0;
        if (pvt != BG_PACK_VT || rm != BG_PACK_REMOVE || s.index < 0 || s.index > 2 || s.index == pcur) {
            BGS.remove_fail++;
            if (++BGS.refused <= 5) wlog("bag: the puppet's bag (vtable %08x, slot 7 %08x) is not the stock cBioItemPack or index %d is held: nothing taken away", (unsigned)pvt, (unsigned)rm, s.index);
            return;
        }
        ops_enter();
        ((bg_remove_fn)(uintptr_t)rm)((void *)(uintptr_t)pk, (unsigned)s.index);
        ops_leave();
        uint32_t after[BG_SLOTS];
        int ac = -1;
        bg_read(pup, after, &ac);
        int ok = after[s.index] == 0;
        if (ok) BGS.removes++;
        else BGS.remove_fail++;
        if (++BGS.lines <= 60) wlog("bag: the partner's puppet (unit %08x) had a full bag: its gun %08x (index %d, not his) %s to make room for his %08x", (unsigned)pup, (unsigned)s.cls, s.index,
                                    ok ? "taken away the stock way (cBioItemPack_vf7)" : "NOT taken away", (unsigned)bg_peer.cur_cls);
    } else if (s.act == BG_EQUIP) {
        if (!bg_fsm_ok(BG_EQUIPSLOT)) {
            if (++BGS.refused <= 5) wlog("bag: PcsMainNpcChangeEquipSlot's bytes differ: no equip");
            return;
        }
        bg_fsm(BG_EQUIPSLOT, pup, (uint32_t)s.index);
        BGS.equips++;
        if (++BGS.lines <= 60) wlog("bag: the partner's puppet (unit %08x) holds his current gun %08x (bag index %d, was %d)", (unsigned)pup, (unsigned)s.cls, s.index, pcur);
    }
}

static void bag_rx(const uint8_t *b, unsigned len)
{
    bg_msg m;
    BGS.rx++;
    bg_kit kt;
    if (bg_dec_kit(b, len, &kt)) {   /* fix021 kit: the host's start kit of this guest's character (once per level instance) */
        if (g_client && !bg_kit_applied && !bg_kit_have) {
            bg_kitv = kt, bg_kit_have = 1;
            wlog("bag: the host's start kit of this PC's character: knife %08x, ammo %u %u %u %u, herbs %u (taken only if this PC's human has no knife)", (unsigned)kt.knife,
                 (unsigned)kt.ammo[0], (unsigned)kt.ammo[1], (unsigned)kt.ammo[2], (unsigned)kt.ammo[3], (unsigned)kt.herb);
        }
        return;
    }
    if (bg_dec_seed(b, len, &m)) {   /* fix021: the host's seed of this guest's guns (once per level instance) */
        if (g_client && !bg_seed_applied && !bg_seed_have) {
            bg_seed = m, bg_seed_have = 1;
            wlog("bag: the host's save gave this PC's character %08x %08x %08x (taken only if this PC's human has no gun)", (unsigned)m.cls[0], (unsigned)m.cls[1], (unsigned)m.cls[2]);
        }
        return;
    }
    if (!bg_dec(b, len, &m)) {
        BGS.bad++;
        return;
    }
    if (!bg_peer_have || !bg_same(&m, &bg_peer))
        if (++BGS.lines <= 60) wlog("bag: the partner carries %08x %08x %08x, current %08x: his puppet here follows", (unsigned)m.cls[0], (unsigned)m.cls[1], (unsigned)m.cls[2], (unsigned)m.cur_cls);
    bg_peer = m, bg_peer_have = 1, bg_peer_epoch = w1_epoch(&N.w);   /* fix021: w1 delivers only events of its current epoch */
}

static void bag_status(char *out, int size)
{
    snprintf(out, (size_t)size, "world bag (bag1.h): bagsync %d | sent %u (failed %u), received %u (bad %u) | the puppet: archives loaded %u (failed %u), guns added %u (failed %u), equips %u, refused %u, taken away %u (failed %u) | the partner's current %08x",
             f_bagsync, BGS.tx, BGS.tx_fail, BGS.rx, BGS.bad, BGS.loads, BGS.load_fail, BGS.adds, BGS.add_fail, BGS.equips, BGS.refused, BGS.removes, BGS.remove_fail,
             bg_peer_have ? (unsigned)bg_peer.cur_cls : 0u);
    {   /* fix021 (LAB BAGSYNC): the partner's report and its level instance, the puppet's three gun slots, the guns the mirror added in this level instance */
        uint32_t pup = 0, pc[BG_SLOTS];
        int pcur = -1, n = (int)strlen(out);
        memset(pc, 0, sizeof pc);
        if (g_own == 0 || g_own == 1) wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &pup);
        if (pup) bg_read(pup, pc, &pcur);
        unsigned ep = w1_epoch(&N.w);
        if (n < size) n += snprintf(out + n, (size_t)(size - n), " | BAGSYNC report epoch %u level %u %s %08x %08x %08x cur %08x | puppet %08x %08x %08x cur %d | added", bg_peer_epoch, ep,
                                    bg_peer_usable(bg_peer_have, bg_peer_epoch, ep) ? "usable" : "none", (unsigned)bg_peer.cls[0], (unsigned)bg_peer.cls[1], (unsigned)bg_peer.cls[2],
                                    (unsigned)bg_peer.cur_cls, (unsigned)pc[0], (unsigned)pc[1], (unsigned)pc[2], pcur);
        for (int i = 0; i < bg_nadded && n > 0 && n < size; i++) n += snprintf(out + n, (size_t)(size - n), " %08x", (unsigned)bg_added[i]);
    }
}
