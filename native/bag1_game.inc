/*
 * bag1_game.inc - included by world1_game.c after motion1_game.inc: rev1 live round 14 (task 3), each puppet carries and holds its owner's real weapons (bag1.h).
 *   every 500 ms (logic thread, a level being played, a gameplay camera on viewport 0): this PC's own human's three weapon slots and current weapon go to
 *   the other PC (ext CP_W1_BAG) on a change and every 3 s; the received owner's bag is planned against this PC's puppet of him: the archive of a missing
 *   gun is loaded the stock way (sResource [0x11E00FC] +0x30 with DTI_rArchive), the gun added by PcsMainCharacterAddWeapon (FUN_00772500), the owner's
 *   current gun equipped by PcsMainNpcChangeEquipSlot (FUN_00775830); one action per 1.5 s. `world set bagsync 0|1`; `world bag` = counters.
 */
#include "bag1.h"

static volatile int f_bagsync = 1;
/* rev1 live 15 (m15a3 LIVE, r48): the host loaded the three DLC gun archives (dlcrchive\...wp1070 / 10a0 / 10b0, ~2.1 MB, kept for the session) and took the
 * puppet's M92F away; 12 ms after its 1-1 -> 1-2 section change began it died: `MessageBoxA "Fatal error.": Failed open file. ...
ativePC\model\chara\em * em1000\em1000.mod 3`, ExitProcess(1) (round 14 passed that change many times without either). Both stay LAB until a live run clears them (default 0):
 * `world set bagdlc 1` = load the DLC guns' archives for a puppet; `world set bagroom 1` = a full puppet bag gives up a gun (BG_REMOVE). With bagdlc 0 the
 * DLC guns are left out of the owner's list before the plan (the puppet keeps its own gun; nothing is taken away for a gun it cannot get). */
static volatile int f_bagdlc, f_bagroom;
static bg_msg bg_own_last, bg_peer;
static int bg_own_have, bg_peer_have;
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
    bg_own_have = 0, bg_peer_have = 0;   /* the archives stay referenced (bg_arch): a later level finds them loaded */
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
static int bg_load_archive(uint32_t cls)
{
    for (unsigned i = 0; i < bg_narch; i++)
        if (bg_arch_cls[i] == cls) return 1;
    for (unsigned i = 0; i < bg_nnoarch; i++)
        if (bg_noarch[i] == cls) return 0;
    char path[96];
    if (!bg_arc_path(cls, path, sizeof path)) return 0;   /* the stock path of FUN_008A29A0 ("dlc\archive\..." for the DLC guns) */
    if (!bg_arc_exists(cls, path)) {
        if (bg_nnoarch < BG_NOARCH_MAX) bg_noarch[bg_nnoarch++] = cls;
        BGS.load_fail++;
        wlog("bag: gun %08x: the archive %s is not there for this game (the stock check FUN_009B12C0 said no): not loaded, the puppet keeps its own gun", (unsigned)cls, path);
        return 0;
    }
    uint32_t sres = rd32(BG_SRES), vt = sres ? rd32(sres) : 0, fn = vt ? rd32(vt + 0x30) : 0;
    if (!fn || bg_narch >= BG_ARCH_MAX) {
        BGS.load_fail++;
        return 0;
    }
    ops_enter();
    uint32_t a = ((bg_load_fn)(uintptr_t)fn)((void *)(uintptr_t)sres, (void *)(uintptr_t)BG_DTI_ARCHIVE, path, 1);
    ops_leave();
    if (!a) {
        BGS.load_fail++;
        wlog("bag: the archive %s did not load: the puppet does not get class %08x", path, (unsigned)cls);
        return 0;
    }
    bg_arch[bg_narch] = a, bg_arch_cls[bg_narch++] = cls, BGS.loads++;
    wlog("bag: the archive %s is loaded (%08x, the stock sResource load as FUN_0087EA10): the puppet's gun %08x gets its model", path, (unsigned)a, (unsigned)cls);
    return 1;
}

static void bg_fsm(uint32_t fn, uint32_t unit, uint32_t a)
{
    uint32_t param[8] __attribute__((aligned(4)));
    memset(param, 0, sizeof param);
    param[2] = 0x10, param[3] = a, param[4] = 0xFFFFFFFFu, param[5] = 1;   /* the unit is given; class / slot; the bag's free slot; one */
    ops_enter();
    ((bg_fsm4_fn)(uintptr_t)fn)(NULL, param, NULL, (void *)(uintptr_t)unit);
    ops_leave();
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
    if (me && rd32(me + WH_U_KIND) == 1 && bg_read(me, cls, &cur)) {   /* the own human's bag to the other PC */
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
    if (!bg_peer_have || !pup || (int32_t)(now - bg_apply_ms) < (int32_t)BG_APPLY_GAP_MS) return;
    bg_msg want = bg_peer;   /* rev1 live 15: the owner's guns this PC can mirror (no DLC gun unless bagdlc) */
    if (!f_bagdlc) {
        for (int k = 0; k < 3; k++)
            if (bg_is_dlc(want.cls[k])) want.cls[k] = 0;
        if (bg_is_dlc(want.cur_cls)) want.cur_cls = 0;
    }
    uint32_t pc[BG_SLOTS];
    int pcur = -1;
    if (!bg_read(pup, pc, &pcur)) return;
    int free_slot = !pc[0] || !pc[1] || !pc[2];
    bg_step s = bg_plan(&want, pc, pcur, free_slot);
    if (s.act == BG_REMOVE && !f_bagroom) return;   /* (LAB bagroom) */
    if (s.act == BG_NONE) return;
    bg_apply_ms = now;
    if (s.act == BG_ADD) {
        if (!bg_fsm_ok(BG_ADDWEAPON)) {
            if (++BGS.refused <= 5) wlog("bag: PcsMainCharacterAddWeapon's bytes differ: the puppet gets nothing");
            return;
        }
        if (!bg_load_archive(s.cls)) return;
        bg_fsm(BG_ADDWEAPON, pup, s.cls);
        uint32_t after[BG_SLOTS];
        int ac = -1;
        bg_read(pup, after, &ac);
        int ok = bg_find(after, s.cls) >= 0;
        if (ok) BGS.adds++;
        else BGS.add_fail++;
        if (++BGS.lines <= 60) wlog("bag: the partner's puppet (unit %08x) %s his gun %08x (his current %08x): %s", (unsigned)pup, ok ? "now carries" : "did NOT take", (unsigned)s.cls,
                                    (unsigned)bg_peer.cur_cls, ok ? "added by the stock AddWeapon after its archive" : "the add left the bag unchanged");
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
    if (!bg_dec(b, len, &m)) {
        BGS.bad++;
        return;
    }
    if (!bg_peer_have || !bg_same(&m, &bg_peer))
        if (++BGS.lines <= 60) wlog("bag: the partner carries %08x %08x %08x, current %08x: his puppet here follows", (unsigned)m.cls[0], (unsigned)m.cls[1], (unsigned)m.cls[2], (unsigned)m.cur_cls);
    bg_peer = m, bg_peer_have = 1;
}

static void bag_status(char *out, int size)
{
    snprintf(out, (size_t)size, "world bag (bag1.h): bagsync %d | sent %u (failed %u), received %u (bad %u) | the puppet: archives loaded %u (failed %u), guns added %u (failed %u), equips %u, refused %u, taken away %u (failed %u) | the partner's current %08x",
             f_bagsync, BGS.tx, BGS.tx_fail, BGS.rx, BGS.bad, BGS.loads, BGS.load_fail, BGS.adds, BGS.add_fail, BGS.equips, BGS.refused, BGS.removes, BGS.remove_fail,
             bg_peer_have ? (unsigned)bg_peer.cur_cls : 0u);
}
