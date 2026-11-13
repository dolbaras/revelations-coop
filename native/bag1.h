/*
 * bag1.h - rev1 live round 14 (task 3): each PUPPET carries and holds its owner's real weapons, so both PCs show the same guns. Pure (no Windows, no engine):
 * the wire and the plan; bag1_game.inc runs it, tests/test_bag1.c drives it.
 *
 * FACTS (round 13 m13a / round 14 m14a, LIVE bags; Ghidra l14 o24 / o39-o43):
 *   - a level's bags are made at the cut start (FUN_008515C0 -> FUN_0084F080 / FUN_005C7060 -> the item factory FUN_005D0C60): the PC's own human gets its
 *     profile's bag, the other character the level's partner set or nothing: m14a 1-1 host Jill M92F / host's Parker puppet Government; guest Parker 10B0
 *     1070 10A0 / guest's Jill puppet unarmed; 1-3 guest's Jill puppet MP5 M3 Remington: the two PCs never show the same weapons;
 *   - the Raid equip sync (0x697A70 / 0x697B80) carries weapon UNIT serials (Raid weapons exist on every PC): not usable here;
 *   - a weapon added mid-level without its archive has no model (l11a: the client crashed at the first aimed shot, 0x5EA96A); the stock way to have one
 *     mid-level is to load its archive first: sResource [0x11E00FC] vtable +0x30 (DTI_rArchive 0x11E0274, "archive\game\chara\wp\wp%04x\wp%04x", 1)
 *     (FUN_0087EA10 does exactly that for wp1c00 / wp1c20), then the stock FSM actions PcsMainCharacterAddWeapon (FUN_00772500) and
 *     PcsMainNpcChangeEquipSlot (FUN_00775830) on the unit (the lab `bag` tools: proven to add / equip).
 * THE RULE: each PC sends its own human's three weapon slots (bag +4 / +8 / +0xC) and the current weapon's class (ext CP_W1_BAG) on a change and every 3 s;
 * the other PC loads the archive of a class its puppet does not carry, adds it to the puppet's bag (the owner's current one first) and equips the owner's
 * current class there. The puppet's own other weapons stay unless the puppet's three weapon slots are full (below).
 * rev1 live 15 (task 3, FACTS):
 *   - the profile pistols 0x80051070 / 0x800510A0 / 0x800510B0 are DLC guns (dat\DAT_02\dlc\archive\game\chara\wp\wpXXXX\wpXXXX.arc in the game
 *     folder, on both PCs): the stock archive path of a class (FUN_008A29A0) starts with "dlc\archive\" for them (the list of FUN_007EAD00 / FUN_008A29A0:
 *     players 0x800111A0 / 0x800115A0, these three guns, 0x80073000 / 10 / 20), else "archive\"; then "game\chara\wp\wpXXXX\wpXXXX". The stock weapon
 *     loader (FUN_005CE180) asks FUN_008A2D60 = FUN_009B12C0(sResource, DTI_rArchive, path) (the resource found / its file exists) before it loads: the
 *     glue does the same (the nativePC file check of m14e is gone: it refused the DLC guns that the game itself loads);
 *   - the 1-3 cut start gives the PUPPET this PC's own profile bag of that character (m14g / m14f: the host's puppet of the guest's Jill got the N100
 *     profile's MP5 / M3 / Remington / grenade; the guest's own Jill kept her real 10B0 / 1070 / 10A0): only the picture on the other PC, never the
 *     owner's real bag. A FULL puppet bag got nothing. Now (BG_REMOVE): when the owner's gun must come and the puppet's three weapon slots are full, one
 *     puppet gun the owner does not carry and the puppet does not hold is taken away the stock way (cBioItemPack_vf7 0x5C6AB0, the per-item removal of
 *     PcsMainCharacterClearWeapon FUN_00772670), then the next pass adds the owner's gun. Never the held gun, never a knife / grenade / scanner / unarmed.
 */
#ifndef BAG1_H
#define BAG1_H

#include <stdint.h>
#include <string.h>

#define BG_LEN 18          /* ver u8, cur index u8, class u32 x3, current class u32 */
#define BG_SLOTS 15        /* bag words +4 .. +0x3C */
#define BG_WEAPON_MASK 0xF00F0000u
#define BG_WEAPON 0x80050000u
#define BG_REFRESH_MS 3000u
#define BG_APPLY_GAP_MS 1500u

typedef struct { uint8_t cur; uint32_t cls[3], cur_cls; } bg_msg;

static inline int bg_is_gun(uint32_t c)
{   /* a weapon the owner shows: the 0x8005 family, not the unarmed / knife / scanner / grenade classes (they exist on every bag) */
    if ((c & BG_WEAPON_MASK) != BG_WEAPON) return 0;
    unsigned fam = (c >> 8) & 0xFF;   /* (DTI dump) 0x10 handguns, 0x11 magnums, 0x12 machine guns, 0x13 shotguns, 0x14 rifles, 0x16 rocket launchers;
                                         0x18 knives, 0x19 grenades, 0x1B unarmed / scanner / goggle, 0x1C arrows, 0x1D / 0x1E turrets */
    return fam >= 0x10 && fam <= 0x16;
}
static inline unsigned bg_enc(uint8_t *b, const bg_msg *m)
{
    b[0] = 1, b[1] = m->cur;
    memcpy(b + 2, m->cls, 12), memcpy(b + 14, &m->cur_cls, 4);
    return BG_LEN;
}
/* fix021 (0.2.1): version 2 of the same message = the SEED: the bag the HOST's save gave the guest's character (its puppet there at the level start) */
static inline unsigned bg_enc_seed(uint8_t *b, const bg_msg *m)
{
    bg_enc(b, m);
    b[0] = 2;
    return BG_LEN;
}
static inline int bg_dec_seed(const uint8_t *b, unsigned len, bg_msg *m)
{
    if (!b || len != BG_LEN || b[0] != 2) return 0;
    m->cur = b[1];
    memcpy(m->cls, b + 2, 12), memcpy(&m->cur_cls, b + 14, 4);
    return 1;
}
static inline int bg_dec(const uint8_t *b, unsigned len, bg_msg *m)
{
    if (!b || len != BG_LEN || b[0] != 1) return 0;
    m->cur = b[1];
    memcpy(m->cls, b + 2, 12), memcpy(&m->cur_cls, b + 14, 4);
    return 1;
}
static inline int bg_same(const bg_msg *a, const bg_msg *b) { return a->cur_cls == b->cur_cls && !memcmp(a->cls, b->cls, sizeof a->cls); }

/* rev1 live 15: the DLC guns (FUN_007EAD00 / FUN_008A29A0) and the stock archive path of a gun class */
static inline int bg_is_dlc(uint32_t c) { return c == 0x80051070u || c == 0x800510A0u || c == 0x800510B0u; }
static inline int bg_arc_path(uint32_t c, char *out, unsigned cap)
{
    static const char hex[] = "0123456789abcdef";
    const char *pre = bg_is_dlc(c) ? "dlc\\archive\\" : "archive\\";
    char id[5];
    unsigned v = c & 0xFFFFu;
    for (int i = 0; i < 4; i++) id[i] = hex[(v >> (12 - 4 * i)) & 0xF];
    id[4] = 0;
    unsigned n = 0;
    const char *parts[5] = {pre, "game\\chara\\wp\\wp", id, "\\wp", id};
    for (int p = 0; p < 5; p++)
        for (const char *s = parts[p]; *s; s++) {
            if (n + 1 >= cap) return 0;
            out[n++] = *s;
        }
    out[n] = 0;
    return (int)n;
}

/* the plan for the puppet: puppet[] = the classes of its bag words +4.. (0 = empty), pcur = its current index. One action per call (the glue waits
 * BG_APPLY_GAP_MS between actions: the archive load and the add are heavy). */
enum { BG_NONE = 0, BG_ADD, BG_EQUIP, BG_REMOVE, BG_REPLACE };
typedef struct { int act; uint32_t cls; int index; } bg_step;
static inline int bg_find(const uint32_t puppet[BG_SLOTS], uint32_t c)
{
    for (int i = 0; i < BG_SLOTS; i++)
        if (c && puppet[i] == c) return i;
    return -1;
}
/* rev1 live 15: a puppet gun that may go to make room (a weapon slot 0..2, a gun the owner does not carry, not the held one); -1 = none */
static inline int bg_spare(const bg_msg *owner, const uint32_t puppet[BG_SLOTS], int pcur)
{
    for (int i = 0; i < 3; i++) {
        uint32_t c = puppet[i];
        if (!c || i == pcur || !bg_is_gun(c) || c == owner->cur_cls) continue;
        int his = 0;
        for (int k = 0; k < 3; k++) his |= owner->cls[k] == c;
        if (!his) return i;
    }
    return -1;
}
static inline bg_step bg_plan(const bg_msg *owner, const uint32_t puppet[BG_SLOTS], int pcur, int free_weapon_slot)
{
    bg_step s = {BG_NONE, 0, -1};
    uint32_t want = bg_is_gun(owner->cur_cls) ? owner->cur_cls : 0;
    if (want && bg_find(puppet, want) < 0) {   /* the owner's gun in hand first */
        if (free_weapon_slot) s.act = BG_ADD, s.cls = want;
        else if ((s.index = bg_spare(owner, puppet, pcur)) >= 0) s.act = BG_REMOVE, s.cls = puppet[s.index];   /* rev1 live 15: a full bag makes room */
        return s;
    }
    if (want) {
        int i = bg_find(puppet, want);
        if (i != pcur) s.act = BG_EQUIP, s.cls = want, s.index = i;
        if (s.act) return s;
    }
    for (int k = 0; k < 3; k++) {   /* then the other guns the owner carries, while a weapon slot is free */
        uint32_t c = owner->cls[k];
        if (bg_is_gun(c) && bg_find(puppet, c) < 0) {
            if (free_weapon_slot) {
                s.act = BG_ADD, s.cls = c;
                return s;
            }
            if ((s.index = bg_spare(owner, puppet, pcur)) >= 0) {   /* rev1 live 15 */
                s.act = BG_REMOVE, s.cls = puppet[s.index];
                return s;
            }
        }
    }
    return s;
}

/* fix021 (0.2.1, notes/fix021.md; the r48 death): the mod loads NO archive itself any more. The stock AddWeapon (FUN_005C7060 -> FUN_005D0C60 -> FUN_005CE180)
 * loads a weapon's archive the stock way (FUN_008A2C30, the dlc\ prefix for the DLC guns), refcounted per item in the item manager's 16-slot table
 * ([0x116AB48] +0x2F8 classes, 0x80000000 = empty; a full table leaks the new archive) and releases it at the item's removal (FUN_005D7D00 -> FUN_005CE230)
 * and at the level end (FUN_0042B290 -> FUN_005D08A0). r48 held its own extra references for the whole session.
 *  - only the stock EXISTENCE check (FUN_009B12C0) before an add; a gun whose archive is not on this PC becomes the nearest stock gun of its family
 *    (the DLC handguns 0x800510xx -> the M92F 0x80051000), else nothing;
 *  - an add of a class the 16-slot table does not hold needs a free slot there;
 *  - room in a full puppet bag: the stock REPLACE (AddWeapon with that slot: the bag's FUN_005C7060(slot, class) releases the old item the stock way and
 *    puts the new one there), never the held gun, never index 3.. (knife, grenade, scanner, unarmed). */
#define BG_STOCK_HANDGUN 0x80051000u
#define BG_ITEM_EMPTY 0x80000000u
static inline uint32_t bg_fallback(uint32_t cls, int exists)
{
    if (exists) return cls;
    if ((cls & 0xFFFFFF00u) == 0x80051000u && cls != BG_STOCK_HANDGUN) return BG_STOCK_HANDGUN;   /* a handgun the PC has no files of: the stock M92F */
    return 0;
}
static inline int bg_item_table_ok(const uint32_t tab[16], uint32_t cls)
{
    for (int i = 0; i < 16; i++)
        if (tab[i] == cls) return 1;
    for (int i = 0; i < 16; i++)
        if (tab[i] == BG_ITEM_EMPTY) return 1;
    return 0;
}
/* 0.2.1: bg_plan with the stock replace instead of a separate removal (cls = the gun to put there, index = the slot it replaces) */
static inline bg_step bg_plan2(const bg_msg *owner, const uint32_t puppet[BG_SLOTS], int pcur, int free_weapon_slot)
{
    if (owner->cur_cls && !bg_is_gun(owner->cur_cls)) {   /* fix021: no gun in his hand (unarmed, knife, grenade, scanner): the puppet holds the same when it carries it */
        int i = bg_find(puppet, owner->cur_cls);
        if (i >= 0 && i != pcur) return (bg_step){BG_EQUIP, owner->cur_cls, i};
    }
    bg_step s = bg_plan(owner, puppet, pcur, free_weapon_slot);
    uint32_t cw = bg_is_gun(owner->cur_cls) ? owner->cur_cls : 0;
    int held_his = pcur >= 0 && pcur <= 2 && (puppet[pcur] == owner->cls[0] || puppet[pcur] == owner->cls[1] || puppet[pcur] == owner->cls[2]);
    if (s.act == BG_NONE && cw && bg_find(puppet, cw) < 0 && !free_weapon_slot && pcur >= 0 && pcur <= 2 && bg_is_gun(puppet[pcur]) && !held_his) {
        /* dw1 live (fix021): a full puppet bag whose only gun the owner does not carry is the HELD one (he swapped his held gun for a new one): the puppet
         * first takes another of its guns in hand, the next pass replaces the old one the stock way (never the held gun) */
        for (int j = 0; j < 3; j++)
            if (j != pcur && bg_is_gun(puppet[j])) return (bg_step){BG_EQUIP, puppet[j], j};
    }
    if (s.act != BG_REMOVE) return s;
    uint32_t want = bg_is_gun(owner->cur_cls) && bg_find(puppet, owner->cur_cls) < 0 ? owner->cur_cls : 0;
    for (int k = 0; k < 3 && !want; k++)
        if (bg_is_gun(owner->cls[k]) && bg_find(puppet, owner->cls[k]) < 0) want = owner->cls[k];
    if (!want || s.index < 0 || s.index > 2 || s.index == pcur) return (bg_step){BG_NONE, 0, -1};
    s.act = BG_REPLACE, s.cls = want;
    return s;
}

/* fix021 (0.2.1), the unarmed guest after a Continue (variant (i) of the lead): the host sends ONCE per level instance the guns its save gave the guest's
 * character (its puppet at the level start, before any mirror change); the guest adds them with the stock AddWeapon ONLY while its own human carries no
 * gun and has carried none in this level instance (a gun picked up / given / dealt by the stock start = never), once, guns only (weapon slots 0..2, no
 * ammunition, no item), each under the weapons rules (the existence check / the stock fallback, the 16-slot item table, not twice). */
static inline int bg_has_gun(const uint32_t bag[BG_SLOTS])
{
    for (int i = 0; i < 3; i++)
        if (bg_is_gun(bag[i])) return 1;
    return 0;
}
/* fix021 (fw1 on de2598fc, 2026-11-12 23:04: the guest's game ended at the 1-2 start, the assert '(reg < Region::RegionNum)' of uEnemyBase.cpp 0x580 when
 * an enemy took a hit at the level start's cutscene): the session's barrier opened by its TIMEOUT during the 1-2 intro movie (207 s), 13.8 s BEFORE this
 * PC's level start; the bag pass (gated by the gameplay camera only) then made the puppet's add, the gun seed and the kit on humans still standing at
 * 0,0,0 (story1's own_placed: e1w2), before the stock level start. In every other run the barrier opened after LOAD_DONE, so every add came after the
 * level start (dw1 13:50, fw1 14:59, flca 21:58 / 23:13, f21r5 22:27: no crash). THE RULE: no bag change of the mod (the mirror, the seed, the kit,
 * Hunk's start gun) before this PC's own human is placed by the stock level start, and the seed / kit settle (3 s) counts from that placement. */
static inline int bg_gate(int cam_gameplay, int own_placed) { return cam_gameplay && own_placed; }
static inline int bg_settled(uint32_t placed_ms, uint32_t now) { return placed_ms != 0 && (uint32_t)(now - placed_ms) >= 3000u; }
/* fix021 (R1 f21r1 2026-11-13 04:13, the host's log): at the 1-2 level start the host's puppet of Parker got 800510b0 from the guest's 1-1 report
 * ('now carries his gun 800510b0' 04:13:04.454) 29 ms before the guest's fresh 1-2 report (80051010 only, 04:13:04.483), and kept it (S2: the puppet
 * Government + 10B0, the guest's own Parker Government only; the host's seed then named 80051010 800510b0). CAUSE in the glue: world_frame runs bag_pass
 * BEFORE the level instance's reset (local_reset -> bg_reset) in the first frame of a new epoch, so the last report of the old level was applied to the new
 * level's puppet; and the plan takes a gun away only when the puppet's three slots are full. THE RULES: (1) a partner report counts only in the level
 * instance (session epoch) it arrived in; (2) a gun the MIRROR added to the puppet in this level instance that the partner's report no longer lists is
 * taken away the stock way (never the held one, never a gun the partner lists, never the puppet's own guns of the level start, never a non-gun). */
static inline int bg_peer_usable(int have, unsigned peer_epoch, unsigned epoch) { return have && peer_epoch == epoch; }
#define BG_ADDED_MAX 8
/* the index (0..2) of a mirror-added gun of the puppet that the owner (as this PC can show him) no longer carries, -1 = none */
static inline int bg_stale_mirror(const bg_msg *owner, const uint32_t puppet[BG_SLOTS], int pcur, const uint32_t added[BG_ADDED_MAX], int nadded)
{
    for (int k = 0; k < 3; k++) {
        uint32_t c = puppet[k];
        if (!c || !bg_is_gun(c) || k == pcur) continue;
        int listed = c == owner->cur_cls;
        for (int j = 0; j < 3; j++) listed |= owner->cls[j] == c;
        if (listed) continue;
        int mine = 0;
        for (int j = 0; j < nadded && j < BG_ADDED_MAX; j++) mine |= added[j] == c;
        if (mine) return k;
    }
    return -1;
}
static inline int bg_seed_wanted(int is_client, int have_seed, int applied, int own_has_gun, int armed_seen, int settled)
{
    return is_client && have_seed && !applied && !own_has_gun && !armed_seen && settled;
}

/* fix021 kit (2026-11-12, offline; for the lead's decision): version 3 of CP_W1_BAG = the START KIT the host's level start gave the guest's character (its
 * puppet there): the knife (bag item 10, +0x2C), the reserve ammo mBullet[12] (+0x40) and the herbs mHerbNum (+0xC8).
 * FACTS: the stock level load gives the human that is LOCAL at the load the bag this PC's game holds for the player (stage6 #84), the other human the level's
 * partner set. On the guest's PC the local human is the guest's character: at the session's FIRST level (the New Game / Continue follow, the stock New Game
 * init ran on the host only) it gets the guest's own title-state profile bag or its own save's bag of that character, without knife, reserve ammo or herb
 * (fr1 B0 1-1: the guest's Parker 10B0 1070 10A0 + unarmed, the host's puppet Government + KnfChris + 30 rounds + 1 herb; flca 3-1: the guest's Jessica
 * nothing, the host's puppet PC356 + KnfChris + 9 / 7 rounds). The knife attack's gate FUN_006ADF20(unit, 1) asks the pack's item 10 (vf14(10)) and
 * refuses without it: no melee. From the first story section change on, the section's start set fills both PCs alike (fr1 S2).
 * THE RULE (the guest; the seed's rules): once per level instance, 3 s after the stock start, only when the kit HAS a knife (the stock start set gives this
 * character one) and the guest's own human has NO knife and had none in this level instance (the stock game never takes a knife away: such a bag is one the
 * stock start never filled): the kit's knife into item 10 (the stock AddWeapon with slot 10), and each count that is 0 on the guest and > 0 in the kit is
 * set to the kit's (a count the guest has is never changed: nothing is lowered or added on top). */
#define BK_AMMO 12
#define BK_LEN (1 + 4 + 2 * BK_AMMO + 2)   /* ver 3, the knife class u32, mBullet[12] u16, mHerbNum u16 = 31 bytes */
#define BK_KNIFE_ITEM 10
typedef struct { uint32_t knife; uint16_t ammo[BK_AMMO]; uint16_t herb; } bg_kit;
typedef struct { uint32_t knife; int ammo[BK_AMMO]; int herb; int sets; } bg_kit_plan;   /* knife 0 = none; a count -1 = kept */

static inline int bg_is_knife(uint32_t c) { return (c & BG_WEAPON_MASK) == BG_WEAPON && ((c >> 8) & 0xFF) == 0x18; }
static inline unsigned bg_enc_kit(uint8_t *b, const bg_kit *k)
{
    b[0] = 3;
    memcpy(b + 1, &k->knife, 4), memcpy(b + 5, k->ammo, 2 * BK_AMMO), memcpy(b + 5 + 2 * BK_AMMO, &k->herb, 2);
    return BK_LEN;
}
static inline int bg_dec_kit(const uint8_t *b, unsigned len, bg_kit *k)
{
    if (!b || len != BK_LEN || b[0] != 3) return 0;
    memcpy(&k->knife, b + 1, 4), memcpy(k->ammo, b + 5, 2 * BK_AMMO), memcpy(&k->herb, b + 5 + 2 * BK_AMMO, 2);
    return 1;
}
static inline int bg_kit_wanted(int is_client, int have_kit, int applied, int kit_has_knife, int own_has_knife, int knife_seen, int settled)
{
    return is_client && have_kit && !applied && kit_has_knife && !own_has_knife && !knife_seen && settled;
}
static inline bg_kit_plan bg_kit_plan_of(const bg_kit *k, const uint32_t own[BG_SLOTS], const int own_ammo[BK_AMMO], int own_herb)
{
    bg_kit_plan p;
    memset(&p, 0, sizeof p);
    p.knife = bg_is_knife(k->knife) && !bg_is_knife(own[BK_KNIFE_ITEM]) ? k->knife : 0;
    for (int i = 0; i < BK_AMMO; i++) {
        p.ammo[i] = own_ammo[i] == 0 && k->ammo[i] > 0 ? (int)k->ammo[i] : -1;
        if (p.ammo[i] >= 0) p.sets++;
    }
    p.herb = own_herb == 0 && k->herb > 0 ? (int)k->herb : -1;
    if (p.herb >= 0) p.sets++;
    return p;
}

#endif
