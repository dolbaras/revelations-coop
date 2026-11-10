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
enum { BG_NONE = 0, BG_ADD, BG_EQUIP, BG_REMOVE };
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

#endif
