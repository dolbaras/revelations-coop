/*
 * world1_hooks.h - the pure part of the engine hooks of the shared world (world1.c) for Revelations 1 (build 13298487): the table of hook sites with the
 * bytes that must be there, a tiny x86 length decoder that proves the stolen prologue ends on an instruction boundary, the detour / trampoline builders,
 * apply / revert with a refusal reason, the readers of the sArrange lists (key <-> unit) and the synthetic HitInfo. No Windows here: the game glue
 * (world1_game.c) points the g1_mem table at the process, the offline test (tests/test_world1_game.c) at a buffer or the real executable image.
 *
 * Evidence for every address: notes\net-world-impl.md section 5, net-world-enemies-re.md section 1, net-items-doors-re.md. The bytes in the table were
 * compared with the image of the Steam build (work\image\rerev-image.bin) when this file was written, and the test repeats that comparison when the image is there.
 *
 * Nothing is written when the bytes are not exactly the expected ones; the reason is kept per site and shown by `world status`.
 */
#ifndef WORLD1_HOOKS_H
#define WORLD1_HOOKS_H
#include <stddef.h>
#include <stdint.h>

#include "guards1.h"   /* g1_mem: the memory table (read / write / alloc / log) shared with the other patch modules */

typedef g1_mem wh_mem;

/* hook sites (detoured) and call sites (only called: their bytes are verified before the glue may call them) */
enum wh_id {
    WH_SPAWN = 0,    /* sArrange::SpawnChara  0x4688F0  thiscall(sArrange; block, index, owner, flags, fromHost, filter) ret 0x18, void */
    WH_APPLYHIT,     /* ApplyHit              0x6CD270  thiscall(victim; HitInfo*) ret 4, eax = hit result */
    WH_ONHIT,        /* OnHit                 0x6CCF30  thiscall(victim; HitInfo*) ret 4 (death when hp <= 0) */
    WH_ADDITEM,      /* cBioItemPack_vf6      0x5CA480  thiscall(pack; 0xF, uItem*, 0) ret 0xC */
    WH_KILLREC,      /* kill record           0x89ECA0  thiscall(sRankManagerResult; cls, serial, killer, victim, event, flag) ret 0x18 */
    WH_FLAG,         /* SetScenarioFlag       0x721DE0  thiscall(flags; id, value) ret 8 */
    WH_DEATH87,      /* cDeathEnemy sender    0x87BC60  thiscall(coop component; victim) ret 4: called after the kill record on all three death paths (raid-reuse.md 4) */
    WH_ZONEHIT,      /* round 9: cEventCollision::HitCheck 0x6D5980  thiscall(collision; record) ret 4, al = hit: the story's zones count the partner only by his PC's report (zone1.h) */
    /* ep1-solo (E3 / E9): the FSM actions that pick "the partner" as "the player unit that is not the own slot" (0x41BC10) or "main" as the own slot's unit (0x40EEF0).
     * The story means slot 1 = Parker / slot 0 = Jill; on the PC that plays the partner the own slot is 1, so the stock code would act on the lead. The hooks run the
     * stock action with the own-slot query answering 0 for this thread (capture_own_scope). All thiscall(action; param, x) ret 8. Not core: each is optional. */
    WH_ENPART,       /* FsmSetEnablePartner        0x752820  param+4 = enable: partner unit +0xC bits 0x400 | 0x800 (update + draw) and its weapon (0x5C58A0) */
    WH_PWARP,        /* FsmSetPartnerWarp          0x7534E0  partner think manager +0x1630, +0x148 bit 3 */
    WH_PATK,         /* FsmSetPartnerAttackStatus  0x753590  partner think manager */
    WH_PLACE,        /* FsmSetPlayerPlaceStatus    0x7536B0  mChara 0 main / 1 partner / 2 both / 3 / 4 host's / guest's: place status 0x6A8AE0 (rain) */
    WH_GSRCH,        /* PcsSetPartnerGenesisSearch 0x776C80  partner think 0x618B00 (the AI's scan target) */
    WH_GSRST,        /* PcsResetPartnerGenesisSearch 0x776D10 */
    WH_PCSACT,       /* ep1-probe E8: mPcsActionStatus getter 0x75D390  thiscall(cFsmActionPcs record; k) ret 4: the prompt status the FSM waits for (command + 0xC) */
    WH_KEYFLAG,      /* rev1 live 10: the key items' scenario flag 0x721F90  thiscall(sEventManager [0x116A344]; item code, value) ret 8 (AddItem 0x5CB004, FUN_005C8560):
                        the flag of a key item (the barn key 0x80041500 -> SF 4864) is stored with FUN_00721920 directly, SetScenarioFlag 0x721DE0 never sees it */
    /* rev1 live 11 (local1.h): where an enemy's unit goes on a PC whose human is far away, and the units an engine re-creates without SpawnChara */
    WH_ZLEAVE,       /* FUN_0044F510  thiscall(sArrange; unit, char kill) ret 8: FUN_004592E0 calls it for a unit that goes to sleep outside the own human's zone (a corpse
                        in state type 12 is killed there with kill = 1 and its record used up; a live one gets its reset state): observed (log) */
    WH_REARR,        /* FUN_00464320  thiscall(sArrange; +0x10, +0x14, +0x38, +0x3C of the record) ret 0x10: ArgCharaRearrange (0x74BA60) re-creates the missing unit of
                        a record that is not used up, with the record's owner and NO SpawnChara (l10e: the guest's 0/0 came back as kind 2); the client's glue makes
                        such a unit a replica when the host simulates it (local1.h); also the client's way to place a host enemy whose record is placed already */
    WH_RMARR,        /* FsmRemoveArrangeChara exec 0x74D3D0  thiscall(action; param{+8 list, +0xC data, ...}, x) ret 8: observed (log) */
    WH_ZPASS,        /* FUN_00460250  thiscall(sArrange) plain ret, every frame from sArrange_vf6: the zone of the OWN human's unit (+0xE2C == the own slot,
                        FUN_00882D60) -> FUN_004592E0 wakes the units of that zone, hides the others (E1C 0x1000). `world set watchzone 1` (LAB, default 0): while
                        this PC WATCHES (its human switched off by the story) the pass runs with the own slot answering the watched human's slot */
    WH_STAGEFL,      /* FUN_004176B0  thiscall(the stage system) plain ret, returns AL 1 = system+0x40..+0x4C hold the 4 stage part codes of the floor under the OWN
                        player (FUN_0040EEF0: +0xE2C == FUN_0087E1B0), 0 = no floor; called by FUN_004177D0 (cSystem_vf15 0x4178F0) every frame, which then loads the
                        listed parts and unloads the others. Rule (stage1.h, `world set watchstage 0|1`, default 1): each PC sends its own human's codes
                        (CP_W1_STAGE); the PC that WATCHES answers with the watched human's codes (the void of l10e / l11a / l12a / l12b: 257 never streamed there) */
    WH_N_HOOKS,
    WH_C_DAMAGE = WH_N_HOOKS,   /* uCharaBase::Damage 0x6CA830 thiscall(unit; float) ret 4 */
    WH_C_ITEMGONE,   /* FUN_005D7D00: take a world item away  thiscall([0x116AB48]; uItem*) ret 4 */
    WH_C_SETKIND,    /* 0x6CD4E0: control kind of a unit (1 human, 2 own AI, 3 remote)  thiscall(unit; kind) */
    WH_C_DMGRECV,    /* 0x49AA50: cDamageSyncData applier  thiscall(component{+0x50 = unit}; msg, slot) ret 8 (no network check, no attacker-kind check) */
    WH_C_VF15,       /* 0x8CC9D0: cUnit::kill (vtable slot 15)  thiscall(unit) ret: state 1/2 -> 3 (removed by the engine) */
    WH_C_FALLOUT,    /* 0x455710: cFallOutData applier  (msg{+8 = serial}, slot) ret 8: vf13 + vf15 + entry+0xD8 = 1; asserts [0x116FFF4] (sTigerTrap) */
    WH_C_TGT,        /* 0x6CD6B0: interpolator target  thiscall(unit; pos*, ang*, flag) ret 0xC (the Raid position applier 0x49A200 calls it with flag 1) */
    WH_C_EULER,      /* 0x9CF880: euler angles of a unit from its quaternion +0x50  thiscall(unit; out*) */
    WH_C_CHGSTATE,   /* 0x49C950: cCharaStateManager::ChangeState  thiscall(mgr = [unit+0xE80]; DTI*, arg, PointerArgs*) ret 0xC */
    WH_C_CURSTATE,   /* 0x49C3E0: type id of the current state  thiscall(mgr) ret: [mgr+0x14]->getDTI()+0x1C */
    WH_C_VITAL,      /* 0x697CD0: cPlayerVitalSyncData applier  thiscall(component{+0x50 = unit}; msg, slot) ret 8 */
    WH_C_DOOROPEN,   /* 0x62E360: uDoorBase open request (the FSM SetOpenDoor command 0x7714C0)  thiscall(door; char side, char b) ret 8: +0xFB4 |= 4, +0xFE0 = mode;
                        the door's own ClosedState update then enters OpeningAuto (index 3) with that mode (1/2: +0xFB4 bit 0 set, motion 4; 3/4: bit 0 clear, motion 5) */
    WH_C_DOORCLOSE,  /* 0x62E3A0: uDoorBase close request (the same FSM command 0x7714C0 with "open" 0)  ECX = door, plain ret: +0xFB4 |= 8. Only OpenedState (vtable
                        0x1031F94) reads it: slot 8 0x62A060 clears the bit and sets the state request 1, slot 9 0x62A090 then changes to index 5 (ClosingState, ctor
                        0x62A0E0). A door opened by the open request stays Opened until this (wp9: the partner's copy still Opened 79 s later) */
    WH_C_FLAGGET,    /* 0x721E30: scenario flag getter  thiscall([0x116A344]; id) ret 4, al = the value (the same lock as SetScenarioFlag): the story mirror's old values and digest */
    WH_N_SITES
};

#define WH_SIG_MAX 24
#define WH_ALT_MAX 8
typedef struct {
    const char *name;
    uint32_t addr;
    int sig_n;                     /* number of bytes that must match at addr */
    uint8_t sig[WH_SIG_MAX];
    int steal;                     /* bytes copied into the trampoline (whole instructions); 0 = a call site */
    const char *what;              /* one line for the status */
} wh_site;
extern const wh_site wh_sites[WH_N_SITES];

/* ---- the x86 length decoder (the subset the stolen prologues use) ---- */
/* length of the instruction at p (at most `avail` bytes readable); 0 = unknown / truncated. *rel = 1 when the instruction is position dependent
 * (call / jmp / jcc rel, ret, indirect call): such an instruction must never be stolen into a trampoline. */
int wh_insn_len(const uint8_t *p, int avail, int *rel);
/* 1 = the first `steal` bytes of code are whole instructions and none of them is position dependent; else why (short text) */
int wh_steal_ok(const uint8_t *code, int avail, int steal, char *why, size_t why_size);

/* ---- detour builders ---- */
/* trampoline: the stolen bytes followed by `jmp back_to` (E9 rel32) placed at tramp_addr; returns the length (steal + 5) */
int wh_tramp_build(uint8_t *out, const uint8_t *stolen, int steal, uint32_t tramp_addr, uint32_t back_to);
/* patch for the site: `jmp hook` (E9 rel32) at addr, the remaining stolen bytes up to `steal` become NOPs (0x90) */
void wh_patch_build(uint8_t *out, int steal, uint32_t addr, uint32_t hook);

/* ---- state ---- */
enum { WH_OFF = 0, WH_ON, WH_REFUSED, WH_ERROR };
typedef struct {
    int mode;                      /* G1_MODE_AUTO / ON / OFF */
    int env_on;                    /* REV1_WORLD=1 */
    int state[WH_N_SITES];         /* hook sites: WH_OFF / ON / REFUSED / ERROR; call sites: WH_ON = bytes verified, WH_OFF = not checked yet */
    char why[WH_N_SITES][112];     /* the reason of the last refusal */
    uint32_t tramp[WH_N_HOOKS];    /* trampoline of a site (allocated once, kept for the life of the process) */
    uint32_t hook_va[WH_N_HOOKS];  /* where the site jumps while ON */
    unsigned applies[WH_N_SITES], reverts[WH_N_SITES], refusals[WH_N_SITES];
    uint8_t alt[WH_N_SITES][WH_ALT_MAX];   /* call sites: a known detour another module of this DLL wrote at the site's entry (downed1 on Damage) */
    int alt_n[WH_N_SITES];                 /* 0 = none */
    int alt_seen[WH_N_SITES];              /* the last check accepted the site because of that detour */
} wh_t;
/* a call site whose first n bytes are another module's detour (downed1's `jmp hook; nop x3` on Damage 0x6CA830, world-downed merge): the check accepts
 * the site when cur[0..n-1] equals exactly these bytes and cur[n..] equals the rest of the signature; any other foreign byte is still refused.
 * The world then calls the site THROUGH that detour (downed1 acts on humans only, enemies and replicas reach the stock code). n = 0 clears. */
void wh_set_alt(wh_t *w, int id, const uint8_t *bytes, int n);

void wh_init(wh_t *w, int env_on);
int wh_want(const wh_t *w, int net_active);                       /* 1 = the hooks should be on now (same rules as the guards: ON mode, else REV1_WORLD=1 or an active network session; OFF mode never) */
int wh_check(wh_t *w, const wh_mem *m, int id);                   /* bytes at the site are the expected ones (or our own patch): 1; else refuse, why is set. A call site becomes WH_ON when ok. */
int wh_prepare(wh_t *w, const wh_mem *m, int id);                  /* check the bytes and make the trampoline (nothing is patched): w->tramp[id] is then the address of the original function for the hook to call; do this BEFORE wh_apply so no thread can enter the hook without it */
int wh_apply(wh_t *w, const wh_mem *m, int id, uint32_t hook_va); /* 1 = the site is ON now (already on counts); safe to call every frame */
int wh_revert(wh_t *w, const wh_mem *m, int id);                  /* only when the live bytes are exactly our patch */
int wh_probe(const wh_t *w, const wh_mem *m, int id, char *out, size_t size);   /* "original" / "patched (jmp XXXXXXXX)" / "FOREIGN xx xx ..." / "unreadable"; returns WH_OFF / WH_ON / WH_REFUSED / WH_ERROR */
void wh_status(const wh_t *w, const wh_mem *m, int net_active, char *out, size_t size);
const char *wh_state_name(int st);

/* ---- engine structures (all reads through the memory table, so a test can fake them) ---- */
#define WH_SARRANGE_PTR   0x11695C4u   /* [..] = the sArrange singleton */
#define WH_SPLAYER_PTR    0x1169394u   /* [..] = the player manager: +0x30 / +0x34 list head / end, node +8 next, node +0x10 unit */
#define WH_ITEMMGR_PTR    0x116AB48u   /* [..] = ecx of FUN_005D7D00 */
#define WH_FLAGMGR_PTR    0x116A344u   /* [..] = ecx of SetScenarioFlag */
#define WH_SARR_BLOCKS    0x28u        /* [sArrange+0x28] = block list: +0x74 count, +0x80 array of block pointers */
#define WH_LIST_ENEMIES   0x80u        /* sArrange+0x80: list of placed enemies (class 0x8003xxxx); +4 count, +0x10 array of entry pointers */
#define WH_LIST_ITEMS     0xA8u        /* sArrange+0xA8: world items (0x8004xxxx) */
#define WH_LIST_OBJECTS   0xBCu        /* sArrange+0xBC: objects / doors (0x8006xxxx), not hooked yet */
#define WH_HIT_SIZE       0x100u
#define WH_HIT_VTBL       0xFF7150u    /* cFightCollision::HitInfo */
#define WH_U_CLASS        0xE28u
#define WH_U_SERIAL       0xE2Cu       /* players: the slot */
#define WH_U_KIND         0xE30u       /* 1 human, 2 own AI, 3 remote */
#define WH_U_HP           0xE38u
#define WH_U_FLAGS        0xE1Cu
#define WH_U_POS          0x40u        /* x y z floats */
#define WH_U_MACHINE      0xE80u       /* cCharaStateManager; +0x3C state number */
#define WH_U_BAG          0xFE8u
#define WH_BAG_OWNER      0xD0u

typedef struct {
    uint32_t entry, unit;          /* the list entry and the object it points to (entry+4) */
    uint32_t k[4];                 /* entry +0x10, +0x14, +0x38, +0x3C: the stable key of a placed object */
} wh_entry;

int wh_rd32(const wh_mem *m, uint32_t addr, uint32_t *out);
int wh_rdf(const wh_mem *m, uint32_t addr, float *out);
int wh_block_ids(const wh_mem *m, uint32_t sarr, unsigned block, uint32_t out[3]);            /* the block's (+0x10, +0x14, +0x1C) */
int wh_char_class(const wh_mem *m, uint32_t sarr, unsigned block, unsigned index, uint32_t *cls);   /* class hash of the placement record (record +8) */
unsigned wh_list_count(const wh_mem *m, uint32_t sarr, uint32_t list_off);                    /* 0 when unreadable or absurd (> 4096) */
int wh_list_at(const wh_mem *m, uint32_t sarr, uint32_t list_off, unsigned i, wh_entry *out);
int wh_find_key(const wh_mem *m, uint32_t sarr, uint32_t list_off, unsigned block, unsigned index, wh_entry *out);   /* by SpawnChara's (block, index) */
int wh_find_unit(const wh_mem *m, uint32_t sarr, uint32_t list_off, uint32_t unit, wh_entry *out);
int wh_find_key4(const wh_mem *m, uint32_t sarr, uint32_t list_off, const uint32_t k4[4], wh_entry *out);
int wh_entry_key(const wh_mem *m, uint32_t sarr, const wh_entry *e, unsigned *block, unsigned *index);   /* the (block, index) a SpawnChara call used for that entry */
/* the same without re-reading the block list for every entry: read the table once per pass, then match entries against it */
typedef struct { uint32_t ids[3]; } wh_block;
unsigned wh_block_table(const wh_mem *m, uint32_t sarr, wh_block *out, unsigned max);        /* number of blocks read (0 = none) */
int wh_key_in_table(const wh_block *t, unsigned n, const wh_entry *e, unsigned *block, unsigned *index);
/* ep1-probe: the enemy of (block, index) is still in the enemy list as the host pass counts it present (an entry with that key whose unit is an enemy, or a
 * parked entry - no unit - not used up whose placement record is an enemy). A fresh read (the block ids by index, then the list): the guard of every DESPAWN,
 * so a stale or unreadable block table never makes the host report a live enemy gone. out (may be NULL) = the entry found. */
int wh_key_listed(const wh_mem *m, uint32_t sarr, unsigned block, unsigned index, wh_entry *out);

uint32_t wh_unit_class(const wh_mem *m, uint32_t unit);     /* unit+0xE28; 0 when unreadable or 0x80000000 (class given by the vtable: not followed) */
int wh_class_is_enemy(uint32_t cls);                         /* (cls & 0xF00F0000) == 0x80030000 */
int wh_class_is_player(uint32_t cls);                        /* ... == 0x80010000 */
int wh_player_by_slot(const wh_mem *m, unsigned slot, uint32_t *unit);   /* the human unit with unit+0xE2C == slot */
/* rev1 live 10: the scenario flag of a key item, read the way FUN_00721F90 reads it: only codes (code & 0xF00FFF00) == 0x80041500; [evmgr+0x2C] = the
 * rEventManage resource (game\episode\setup), its rKeyItem table [res+0x90] = count, [res+0x9C] = entries, index = code & 0xFF, entry+8 = the flag (-1 = none).
 * 1 = *flag set. FACT (game.arc game\episode\setup): 0x80041500 (uItKeyBarn it1500) -> 4864, 0x80041501 -> 8704. */
#define WH_KEYITEM_MASK  0xF00FFF00u
#define WH_KEYITEM_CODE  0x80041500u
int wh_keyitem_flag(const wh_mem *m, uint32_t evmgr, uint32_t code, uint32_t *flag);
/* the human behind a hit (HitInfo +0x40 attacker first, then +0x44 owner): 1 and its unit / slot / kind when it is a player unit, else 0 */
int wh_hit_human(const wh_mem *m, uint32_t hit, uint32_t *unit, int *slot, int *kind);
/* host: a blow whose human (wh_hit_human) is the PARTNER's puppet on an enemy is dropped (1) unless puppethits: his blows count only through his HIT (no double damage, no
 * AI kill). in_ops = the world's own call (a HIT applied for him) passes. */
int wh_partner_blow_dropped(int host, int human, int hslot, int own, int in_ops, int puppethits);
/* host: the killer slot an enemy death is reported with. A kill by the partner's puppet that his pad does not drive (the AI took him back) credits nobody (no_slot);
 * the own human, a HIT applied for him (in_ops) and a pad-driven puppet keep the slot. */
int wh_death_credit_slot(int slot, int no_slot, int own, int in_ops, int pad_driven);

/* a HitInfo the kill chain accepts: vtable, the point (+0x10), attacker (+0x40), owner (+0x44), flags (+0x54); everything else zero */
void wh_hit_build(uint8_t buf[WH_HIT_SIZE], uint32_t vtbl, uint32_t attacker, uint32_t owner, uint32_t flags, const float pt[3]);

/* ---- what a SpawnChara call left behind (live run 1: the client crashed on a NULL unit here) ----
 * PROVEN (disassembly 0x46A609..0x46A646): when the block's area (+0x18) is not the current area, SpawnChara registers the placement entry, then calls the unit's
 * vf15 and its deleting destructor and writes entry+4 = 0. So an entry with a NULL unit is a NORMAL result: "placed, not instantiated here". */
enum { WH_SP_NONE = 0,      /* no entry for (block, index): nothing placed (duplicate refused, difficulty filter, no record) */
       WH_SP_NOUNIT,        /* the entry exists, its unit is NULL (block of another area): there is no unit to touch */
       WH_SP_UNIT };        /* the entry and a readable enemy unit */
int wh_spawn_result(const wh_mem *m, uint32_t sarr, unsigned block, unsigned index, uint32_t *unit);
/* the owner slot to give SpawnChara so that the engine itself makes the unit a REPLICA (kind 3): SpawnChara sets kind 2 when owner == FUN_00882D60() (the engine's own
 * slot) and 3 otherwise (decompile of 0x4688F0, `(**(code **)(*piVar8 + 0xf0))(own == param_4 ? 2 : 3)`). Prefers the host's slot; never the engine's own slot. */
int wh_replica_owner(int engine_own, int host_slot);
/* replica far from the host's sample (wp1 live 2026-11-07: after a teleport / floor change neither the interpolator target nor a plain position write brings it
 * there - writes are swept against the collision): 1 = put it there with the move object's teleport byte. Horizontal distance over thr, or a height difference
 * over WH_SNAP_DY (another floor). thr <= 0 = never; any NaN = never. */
#define WH_SNAP_DY 150.0f
int wh_snap_needed(const float cur[3], const float tgt[3], float thr);
/* the unit's virtual method at byte offset `off` of its vtable: 1 and *fn when readable */
int wh_unit_vfn(const wh_mem *m, uint32_t unit, uint32_t off, uint32_t *fn);
#define WH_VF_SETKIND 0xF0u   /* vtable slot 60: control kind setter; 0x6CD4E0 in 528 vtables of the build (uPlayer has its own 0x6A7B40) */

/* ---- gates of the interpolator cNetInterpMove (byte patches, not detours) ----
 * PROVEN (raid-reuse.md 5.5): the interpolator (+0xECC) exists offline for humans and enemies and drives only units of view control 3, but its tick 0x4A0D90 and its
 * target setter 0x4A09B0 return when Net_IsOnline() is 0: `mov ecx,[0x11E0510]; call Net_IsOnline; test al,al; jz exit` (19 bytes). The patch makes the `jz` a 6-byte NOP. */
/* world-downed (static, 2026-11-07): a THIRD check of the same kind sits in the queue insert 0x49FA40 (`if (!Net_IsOnline()) return;` at 0x49FA58): offline a target
 * that is near enough to be walked to is never queued, so the tick (gate 1) finds the queue +0x10/+0x18 empty and the replica does not move - the live fact of wp1-wp3.
 * Gate 3 makes that `jz` a NOP as well. Still needed besides the gates (not done here): the replica's state id (FUN_0049C430 = [[unit+0xE80]+0x14]+0xC) must accept a
 * target - 0x4A09B0 drops every target in state 0xD, and with flag 1 only states 1, 2 and 6 queue (0x49DBD0); `world list` shows that state id. */
enum { WH_G_TICK = 0, WH_G_TARGET, WH_G_QUEUE, WH_N_GATES };
typedef struct {
    const char *name;
    uint32_t addr;                 /* start of the checked bytes */
    int n;                         /* checked bytes */
    uint8_t orig[24];
    int off;                       /* where the patch goes (addr + off) */
    int pn;
    uint8_t patch[8];
} wh_gate_site;
extern const wh_gate_site wh_gates[WH_N_GATES];
typedef struct {
    int state[WH_N_GATES];         /* WH_OFF / WH_ON / WH_REFUSED / WH_ERROR */
    char why[WH_N_GATES][112];
    unsigned applies[WH_N_GATES], reverts[WH_N_GATES], refusals[WH_N_GATES];
} wh_gates_t;
int wh_gate_apply(wh_gates_t *g, const wh_mem *m, int id);    /* 1 = patched now (already counts); refuses unless the bytes are exactly the original ones */
int wh_gate_revert(wh_gates_t *g, const wh_mem *m, int id);   /* only when the bytes are exactly our patch */
int wh_gate_probe(const wh_mem *m, int id, char *out, size_t size);   /* "original" / "patched" / "FOREIGN ..." / "unreadable" */

/* ---- an enemy's blow on the other PC's human (PCHIT): the cDamageSyncData layout of the Raid ----
 * PROVEN both directions: the sender 0x6CF8C0 copies HitInfo -> msg, the applier 0x49AA50 copies msg -> a new HitInfo:
 *   msg+0x10/+0x20/+0x30 = hit +0x10/+0x20/+0x30 (3 floats each), +0x40/+0x42 = u16 serials of attacker / owner (hit +0x40/+0x44 units), +0x44 = (char) hit+0x48,
 *   +0x45 = (char) +0x4C, +0x48 = +0x50, +0x4C = +0x54 (flags), +0x50 = (u8) +0x58, +0x51 = (char) +0x5C, +0x54..+0x68 = +0x64..+0x78, +0x6C = (char) +0x7C,
 *   +0x70 = +0x80, +0x74 = +0x84, +0x78 = number of extra hits (0 here). */
#define WH_DMGMSG_SIZE 0x90
#define WH_PCHIT_LEN 88            /* msg +0x10..+0x1B, +0x20..+0x2B, +0x30..+0x3B, +0x44..+0x77 */
void wh_dmgmsg_from_hit(const uint8_t hit[WH_HIT_SIZE], uint16_t att_serial, uint16_t own_serial, uint8_t msg[WH_DMGMSG_SIZE]);
void wh_pchit_pack(const uint8_t msg[WH_DMGMSG_SIZE], uint8_t out[WH_PCHIT_LEN]);
void wh_pchit_unpack(const uint8_t in[WH_PCHIT_LEN], uint16_t att_serial, uint16_t own_serial, uint8_t msg[WH_DMGMSG_SIZE]);
#define WH_RAIDCOMP_SIZE 0x54      /* fake Raid component: the appliers read only +0x50 (the unit) */

/* ---- "exactly once" per enemy key (the kill record of the client: 0x89ECA0 counts again on a repeated call because Net_IsHost() = 1 offline) ---- */
typedef struct { uint8_t bits[8192]; } wh_keyset;
void wh_keyset_clear(wh_keyset *s);
int wh_keyset_first(wh_keyset *s, uint16_t key);   /* 1 = first time for this key (now marked), 0 = already marked */

/* ---- door states (uDoorBase): DTI objects made by 0x8E1330(ecx = dti, name, parent 0x1175580, size, 0, 0) at start-up (PROVEN 0xF80D62..):
 * [dti] = 0x102AA00 (MtDTI vtable), [dti+4] = name, [dti+0x10] = parent, [dti+0x1C] = id = crc(name) & 0x7FFFFFFF (the value 0x49C3E0 returns). ---- */
enum { WH_DS_CLOSED = 0, WH_DS_KNOB, WH_DS_OPENING_PLAYER, WH_DS_OPENING_AUTO, WH_DS_OPENED, WH_DS_GIMMICK, WH_N_DS };
typedef struct { const char *name; uint32_t dti, name_str; } wh_door_state;
extern const wh_door_state wh_door_states[WH_N_DS];
#define WH_DTI_VTBL 0x102AA00u   /* the base MtDTI vtable 0x8E1330 writes; LIVE (wp2 2026-11-07): each state's DTI then carries its OWN vtable (01031e64, 01031e9c, ...) */
#define WH_DTI_VT_LO 0x401000u   /* so the check takes any vtable inside the exe image (constructed: not 0), plus name pointer, parent and a non-zero id */
#define WH_DTI_VT_HI 0x1200000u
#define WH_DOORSTATE_PARENT 0x1175580u
int wh_door_dti(const wh_mem *m, int which, uint32_t *id);   /* 1 and the id when the DTI object is the expected one (vtable in the image, name pointer, parent, id != 0) */
/* doors over the network (round 4, static: cDoorBaseState.cpp, ClosedState vf9 0x62A990, KnobHandOn enter 0x62AD80, OpeningPlayer enter 0x62B200, OpeningAuto enter 0x629DC0).
 * uExperimentDoor, uShipDoorUAVRoom02, uRoomDoorWooden have the parent DTI uDoorBase (0x1175F78): the states of the table above. The own human's A: Closed -> KnobHandOnPlayer
 * (index 1, NetParam {own slot, side}) -> OpeningPlayer (index 2); the side is kept in door+0xFB4 bit 0 (set when the side is 0). A script / remote open: 0x62E360(door, a, b)
 * -> the door's ClosedState update enters OpeningAuto (index 3) with mode a ? 1|2 : 3|4 (1/2 set bit 0, motion 4; 3/4 clear it, motion 5) and closes by itself later.
 * Calling ChangeState(OpeningAuto) from outside with arg 0 starts NO motion (0x629DC0: `if (arg != 0)`), the door would wait in OpeningAuto: the old `doors 1` path. */
#define WH_DOOR_FB4 0xFB4u
int wh_door_player_open(const uint32_t ds[WH_N_DS], uint32_t old_id, uint32_t new_id);   /* 1 = this change is the own human opening the door (announce it) */
/* the opener's PC: 1 = a door the own human opened (own_open, set when its opening was announced) is back in ClosedState: announce Closed so the partner closes its copy */
int wh_door_own_closed(const uint32_t ds[WH_N_DS], uint32_t old_id, uint32_t new_id, int own_open);
/* the partner's PC on the opener's Closed: 1 = close the copy here (it is OpeningAuto / Opened = what the open request made of it); Closed, the knob / OpeningPlayer of the own
 * human here, Gimmick or unknown states are left alone */
int wh_door_close_wanted(const uint32_t ds[WH_N_DS], uint32_t here_id);
uint8_t wh_door_phase(uint32_t fb4);                                                       /* the side for the wire: 1 + (fb4 & 1) */
/* the phase byte of an opening: bits 0-1 the side, bits 2-7 a counter one above the phase the model holds for this door (w1_local_door drops an announcement equal to
 * the stored state: a second opening of the same door from the same side - by either PC - would never be sent) */
uint8_t wh_door_phase_next(uint8_t stored_phase, uint32_t fb4);
void wh_door_req_args(uint8_t phase, int *a, int *b);                                     /* the arguments of 0x62E360 for the partner's side (phase 0 = unknown: a 1) */
#define WH_ENTRY_SPENT 0xD8u        /* sArrange entry +0xD8 (byte) = 1: used up, never placed again (0x455710, 0x6F0660, 0x5D6110) */

#endif
