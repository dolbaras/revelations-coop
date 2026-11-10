/*
 * hitx1.h - rev1 live round 14: an enemy the CLIENT kills dies on the host by the game's own death, and its replica dies on the client the same way.
 * Pure decisions and the wire (no Windows, no engine): world1_game.c (hitx1_game.inc) feeds them, tests/test_hitx1.c drives them on a model of the engine.
 *
 * THE DEFECT (round 13 m13f, LIVE): after the client's killing hit the host's Ooze stayed `hp 0 E1C 40000088 st 1` (no dead bit) until the next game over; the
 * host's model marked it dead, the guest's replica went back to hp 1 (later rounds of the burst: the replica branch of OnHit), local1 took that replica to
 * the guest's AI and it bit the guest's Parker to death; 1-1 never ended, 4906 never came in 1-3 (the LAB kills behaved the same).
 * STATICS (Ghidra l13 o15-o22, l14 o1-o5):
 *   - ApplyHit 0x6CD270 (the zombie's slot 56 0x56E360 wraps it) = vf45 (accept) + a HitInfo copy + FUN_0049D3E0 (the unit's HIT QUEUE [unit+0xEA0], a
 *     CRITICAL_SECTION list of 0x100-byte HitInfos) + vf46 (uEnemy_vf46 0x573F90: part damage, reaction set-up FUN_005713B0) + Damage 0x6CA830 + OnHit 0x6CCF30;
 *   - the zombie's update uZombieBase_vf43 0x5BAC70 walks the hit queue (FUN_005B8530 per HitInfo): with hp <= 0 it picks a DEATH state (+0x59F = 0x4A..0x58,
 *     +0x5A0 = the killer's serial << 5 | 0x10); the next update requests it (FUN_0049C790 on [unit+0xE80]) ONLY when the unit is local (FUN_006C9FE0: kind
 *     +0xE30 is not 0 and not 3) and calls FUN_0045CB40 (the record update); DeadState_vf10 0x6676D0 later sets E1C bit 31 (the dead bit); FUN_00457370 marks
 *     the record used up (+0xD8) only with that bit; the story's dead checks (FUN_00459040) read +0xD8;
 *   - op_apply_hit / op_kill / the LAB kill called only Damage + OnHit: NO hit queue entry, so no death state, no dead bit, the record never used up;
 *   - the Raid applier of cDamageSyncData 0x49AA50 (thiscall(component{+0x50 = victim}; msg, slot), ret 8; PCHIT uses it live) builds a HitInfo from the
 *     message (+0x40 / +0x42 = the serials of attacker / owner, looked up in sCharaBase [0x11695C8]+0x34 list by +0xE2C through FUN_006C92A0, kept only in
 *     state 1 / 2), then hit queue + vf46 + Damage + OnHit: ApplyHit without vf45 and without the attacker-kind refusal.
 * THE FIX:
 *   1. the client's blow on a replica sends the REAL HitInfo (the PCHIT packing, 88 bytes) as ext CP_W1_HITX instead of the bare HIT; the host plays it
 *      through the applier with the client's puppet as attacker / owner (serial = the client's slot, checked to resolve to the puppet), so the host's enemy
 *      reacts and dies by its own code (death state -> DeadState -> dead bit -> record used up -> the story); the kill record / credit of OnHit as before;
 *   2. the client's replica on the host's DEATH: made local (kind 2, owner = the engine's own slot, local1's lr_set_kind), its hp 0, the lethal HitInfo
 *      (the client's own last one for that key when its human killed, else one from the DEATH's point and flags) queued through the applier WITHOUT an
 *      attacker (no kill record, no stats: the client's credit stays credit_kill's), so its own update picks the stock death and its record goes used up;
 *   3. a dead enemy that still is not used up after HX_FINISH_MS (no death state came: an unknown class, no hit queue) is finished the way local_rx's
 *      SPEND_KILL does (vf15 + +0xD8 = 1) on either PC;
 *   4. local1 never takes a replica the model already calls dead (local1.h lr_step, LR guard).
 */
#ifndef HITX1_H
#define HITX1_H

#include <stdint.h>
#include <string.h>

#define HX_PAY 88                  /* = WH_PCHIT_LEN: msg +0x10..+0x1B, +0x20..+0x2B, +0x30..+0x3B, +0x44..+0x77 of cDamageSyncData */
#define HX_LEN (4 + HX_PAY)        /* key u16, damage u16 (the client's own vf46 result, for the old path), payload: 92 = W1_EXT_MAX */
#define HX_FINISH_MS 15000u        /* a dead enemy must be used up by then (the longest dying animation of Episode 1 is a few seconds) */
#define HX_E1C_DEAD 0x80000000u    /* DeadState_vf10 0x6676D0 */
#define HX_MAXW 64
#define HX_MAXL 32

/* ---- the wire (ext CP_W1_HITX) */
static inline unsigned hx_enc(uint8_t *b, uint16_t key, float dmg, const uint8_t pay[HX_PAY])
{
    unsigned d = dmg <= 1.0f ? 1u : dmg >= 65535.0f ? 65535u : (unsigned)(dmg + 0.5f);
    b[0] = (uint8_t)(key & 0xFF), b[1] = (uint8_t)(key >> 8), b[2] = (uint8_t)(d & 0xFF), b[3] = (uint8_t)(d >> 8);
    memcpy(b + 4, pay, HX_PAY);
    return HX_LEN;
}
static inline int hx_dec(const uint8_t *b, unsigned len, uint16_t *key, float *dmg, const uint8_t **pay)
{
    if (!b || len != HX_LEN) return 0;
    *key = (uint16_t)(b[0] | (b[1] << 8));
    *dmg = (float)(b[2] | (b[3] << 8));
    *pay = b + 4;
    return 1;
}

/* ---- host: how a client's hit is applied */
enum { HX_P_APPLIER = 1,   /* the Raid applier: hit queue + vf46 + Damage + OnHit (the fix) */
       HX_P_DIRECT,        /* Damage(the client's damage) + OnHit with the puppet as attacker (the old way; the finish of rule 3 follows a death) */
       HX_P_LATE,          /* the model has it dead / gone already: the hit is moot */
       HX_P_NOUNIT };      /* no live unit here */
/* model_alive = the host's model says ALIVE; unit = a unit with hp > 0; applier_ok = the call site's bytes verified and the singleton there; queue_ok =
 * [unit+0xEA0] != 0 (the applier asserts it); serial_ok = the client's slot as a serial resolves to the puppet in the applier's own lookup */
static inline int hx_host_path(int on, int model_alive, int unit, int applier_ok, int queue_ok, int serial_ok)
{
    if (!model_alive) return HX_P_LATE;
    if (!unit) return HX_P_NOUNIT;
    return on && applier_ok && queue_ok && serial_ok ? HX_P_APPLIER : HX_P_DIRECT;
}

/* ---- client: how the replica dies on the host's DEATH */
enum { HX_D_STOCK = 1,     /* local (kind 2) + hp 0 + the lethal HitInfo queued (attacker-less): its own update picks the stock death */
       HX_D_OLD };         /* Damage(hp) + OnHit (attacker-less): hp 0, no death state (rule 3 finishes it) */
static inline int hx_replica_death(int on, int kind, int applier_ok, int queue_ok)
{
    return on && kind == 3 && applier_ok && queue_ok ? HX_D_STOCK : HX_D_OLD;
}

/* ---- both: the finish of a dead enemy that the stock death did not reach (rule 3) */
typedef struct { uint16_t key; uint8_t used, done; uint32_t since; } hx_wd_ent;
typedef struct { hx_wd_ent e[HX_MAXW]; unsigned finished; } hx_wd_t;
static inline void hx_wd_reset(hx_wd_t *t) { memset(t, 0, sizeof *t); }
enum { HX_WD_NONE = 0, HX_WD_FINISH_UNIT, HX_WD_FINISH_RECORD };
/* per pass, for a key the model calls DEAD: unit = its unit here (0 none), hp / e1c of it, spent = the record's +0xD8. Returns what the glue does once. */
static inline int hx_wd_step(hx_wd_t *t, uint16_t key, int model_dead, uint32_t unit, float hp, uint32_t e1c, int spent, uint32_t now, uint32_t limit_ms)
{
    hx_wd_ent *f = 0, *fr = 0;
    for (unsigned i = 0; i < HX_MAXW; i++) {
        if (t->e[i].used && t->e[i].key == key) { f = &t->e[i]; break; }
        if (!t->e[i].used && !fr) fr = &t->e[i];
    }
    if (!model_dead || spent || (unit && (hp > 0.0f || (e1c & HX_E1C_DEAD)))) {   /* alive, the stock death came (dead bit / used up): nothing */
        if (f && !model_dead) f->used = 0;
        return HX_WD_NONE;
    }
    if (!limit_ms) return HX_WD_NONE;
    if (!f) {
        if (!fr) return HX_WD_NONE;
        f = fr, memset(f, 0, sizeof *f), f->used = 1, f->key = key, f->since = now ? now : 1;
        return HX_WD_NONE;
    }
    if (f->done || (uint32_t)(now - f->since) < limit_ms) return HX_WD_NONE;
    f->done = 1, t->finished++;
    return unit ? HX_WD_FINISH_UNIT : HX_WD_FINISH_RECORD;
}

/* ---- client: the last real HitInfo of the own human per key (the replica's death replays it) */
typedef struct { uint16_t key; uint8_t used; uint32_t seq; uint8_t pay[HX_PAY]; } hx_last_ent;
typedef struct { hx_last_ent e[HX_MAXL]; uint32_t seq; } hx_last_t;
static inline void hx_last_reset(hx_last_t *t) { memset(t, 0, sizeof *t); }
static inline void hx_last_put(hx_last_t *t, uint16_t key, const uint8_t pay[HX_PAY])
{
    hx_last_ent *f = 0, *old = 0;
    for (unsigned i = 0; i < HX_MAXL; i++) {
        if (t->e[i].used && t->e[i].key == key) { f = &t->e[i]; break; }
        if (!t->e[i].used) { if (!f) f = &t->e[i]; continue; }
        if (!old || (int32_t)(t->e[i].seq - old->seq) < 0) old = &t->e[i];
    }
    if (!f || (f->used && f->key != key)) f = old;   /* full: the oldest goes */
    if (!f) return;
    f->used = 1, f->key = key, f->seq = ++t->seq;
    memcpy(f->pay, pay, HX_PAY);
}
static inline const uint8_t *hx_last_get(const hx_last_t *t, uint16_t key)
{
    for (unsigned i = 0; i < HX_MAXL; i++)
        if (t->e[i].used && t->e[i].key == key) return t->e[i].pay;
    return 0;
}

/* the killer's own last HitInfo is replayed only when the DEATH names this PC's human as the killer (otherwise the DEATH's point / flags) */
static inline int hx_use_last(int killer_slot, int own_slot, int have_last) { return have_last && killer_slot == own_slot; }

#endif
