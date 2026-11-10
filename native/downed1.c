/*
 * downed1.c - engine glue of the "downed / revive" rule in Revelations 1 (local split: two humans on one PC, no network).
 * The rule is pure (downed_logic.h, downed1_pair.h; offline replay tests/test_downed_pair.c). This file reads the facts (hp, position, the
 * held buttons of the two captured humans from capture.c), feeds them to the pair and applies what the pair asks for.
 *
 * DEFAULTS (ep1-flow E12): in a NETWORK session the rule is ON as the host's lobby row says (Downed / revive: Off, 30 / 60 / 120 s, no time limit; default
 * 60 s), carried to the guest in the session record's flags byte. The local split rule runs only with REV1_DOWNED=1 (lab); REV1_DOWNED=0 switches the module
 * off. The hook is installed only when a rule uses it and only when the bytes at its address are those of the supported build (rerev.exe 13298487).
 *
 * Engine facts (static; docs/downed-revive.md lists what is still unverified live):
 *   Damage(float) = thiscall(unit, float) at 0x6CA830 (ret 4): hp (unit+0xE38) = clamp(hp - dmg, 0, max (unit+0xE40)). One direct call site
 *     (0x6AC10B); the hook sits on the function entry, so every caller passes it.
 *   Hook bytes: f3 0f 10 81 38 0e 00 00 (movss xmm0,[ecx+0xE38]), one whole instruction, enough for the 5-byte jump.
 *   Death: the per-unit damage pass FUN_006B5F40 takes the death path for a queued hit that leaves hp <= 0. Damage() alone does not kill
 *     (stage3.md item 8: hp 0 from Damage is regenerated), so a bleed-out enters the campaign death state 0x32 (downed1_pose.h).
 *   Regeneration (cVitalControl, FUN_006A4D70) raises hp after a hit: held at 1 here while a human is down.
 *   Pose (downed1_pose.h): the game's input ban (unit+0xF3C bit 20), the campaign knock-down 0x2D and its get-up frozen at the first
 *     frame (lying), played on at the revive (the real getting up); hp through the Raid revive function 0x6A5290. The Raid agony itself
 *     (motion mode 2) has no motions in the campaign (live: T-pose). REV1_DOWNED_POSE=0 runs the rule without the pose.
 *   Held logical buttons of pad slot k: sGamePad + 0x3C + 0x2CC*k + 0x15C (probe.c); A = 0x4000 (stage3.md section 1).
 *   Unit position: floats at +0x40, +0x44, +0x48.
 * Threads: Damage can run on any of the unit-phase threads (notes\stage3.md, 8 threads), so the pair is guarded by a critical section and
 * the calls into the game are made outside it.
 *
 * NETWORK MODE (a started session, net1_session_active): one human per PC. This PC runs ONE machine (downed1_net.h, ME = the slot of its own human); the partner's
 * machine runs on the partner's PC and the two talk through reliable CP_EVENT kind CP_EV_D1 (0x142). Only the own human's hits are softened here (the host's enemy
 * blows on the client's puppet are applied on the client: world1 PCHIT). The partner's puppet mirrors him: knock-down + frozen get-up on his DOWN, the get-up on his
 * revive, the death state 0x32 on his death; this PC's human revives him by holding A next to the puppet (the hold is measured here and sent); a death on either PC
 * makes the other PC's rule end its own human too (both PCs reach the game over). The puppet module (puppet_game.c) adds CP_SF_DOWN to this PC's pad / STATE flags
 * while the own human lies, which holds the partner's puppet of him (neutral pad, no position correction); the partner's flags repair a lost DOWN / DONE after 600 ms.
 */
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"
#include "capture.h"
#include "net1.h"
#include "downed1_pair.h"
#include "downed1_net.h"
#include "downed1_pose.h"
#include "downed1.h"
#include "solo1.h"
#include "world1_game.h"   /* ep1-solo: solo1_unit_off */

#define TC __attribute__((thiscall))

#define D1_HP_OFF 0xE38u
#define D1_MAX_OFF 0xE40u
#define D1_POS_OFF 0x40u
#define D1_DAMAGE_ADDR 0x6CA830u
#define D1_BTN_REVIVE 0x4000u      /* logical A */
#define D1_LOG_MAX 300u

static const unsigned char d1_damage_bytes[8] = {0xF3, 0x0F, 0x10, 0x81, 0x38, 0x0E, 0x00, 0x00};

typedef void (TC *d1_damage_fn)(void *self, float dmg);

static CRITICAL_SECTION d1_cs;
static int d1_inited, d1_on;               /* d1_on: the module (ep1-flow: on unless REV1_DOWNED=0; a network session's host decides the rule) */
static int d1_local;                       /* the local split rule (two humans on one PC): REV1_DOWNED=1 only (lab) */
static int d1_hooked, d1_hook_failed;
static void *d1_tramp;                     /* the stock Damage prologue + a jump back into it */
static d1_pair_t d1_pair;
static volatile unsigned d1_units[D1_N];   /* the units the pair was built for (the last non-zero unit of each human) */
static unsigned d1_absent[D1_N];           /* passes without a unit, per human (presence debounce) */
static unsigned d1_nlog;
static int d1_pose_on = 1;                 /* REV1_DOWNED_POSE != 0 */
static int d1_pose_ok = -1;                /* -1 not checked, 0 bytes differ, 1 the pose calls are those of the supported build */
static unsigned d1_flags = CP_SESS_DOWNED; /* + the bleed-out preset of REV1_DOWNED_BLEED (60 default, 30, 120, 0 = never) */
static unsigned d1_kept[D1_N];             /* passes where KEEP had to put something back (log only) */
static d1p_hum d1_ph[D1_N];                /* the pose phase of each human (logic thread only) */
static unsigned char d1_patch_written[8];  /* the 8 bytes this module wrote at 0x6CA830 (world1_hooks accepts them: downed1_damage_patch) */
/* LAB ONLY (test aid, not in the menu or the readme): REV1_LAB_GOD=1 or `downed god 1`. The Damage hook then gives the humans 0 damage (the 9-2 laser trap
 * and the shooter ended wp2 / wp3 before the kill steps) and logs the damage values of humans AND enemies with the caller (double-damage check:
 * a blow of the partner's puppet and the client's HIT on the same enemy show up as two lines on the host). Works without REV1_DOWNED. */
static volatile int d1_god;
static volatile LONG d1_god_skipped, d1_god_enemy;
static volatile LONG d1_god_nlog;

/* network mode (one human per PC) */
static d1n_t d1_net;
static volatile int d1_netmode;            /* 1 while a started session runs: d1_net decides, the pair is idle */
static int d1_net_me = -1;
static volatile unsigned d1_net_own;       /* the own human's unit (the Damage hook softens only this one in network mode) */
static unsigned d1_net_pup;                /* the partner's puppet unit on this PC */
static unsigned d1_net_absent;
static lk_t *d1_reg_link;
typedef struct { uint8_t len; uint8_t b[16]; } d1_evq_t;
static d1_evq_t d1_eq[32];
static int d1_eq_head, d1_eq_tail;
static CRITICAL_SECTION d1_eq_cs;
static uint32_t d1_net_sent, d1_net_send_fail, d1_net_got, d1_net_samples;
int puppet_partner_flags(unsigned *flags);   /* puppet_game.c: the partner's newest pad / STATE flags (CP_SF_*), 1 = fresh */
int puppet_scene_now(unsigned *flags);       /* puppet_game.c: this PC's own scene flags of the last pass (CP_SF_LOADING / CUTSCENE / PAUSED), 1 = fresh */
/* the revive hold (round 4, downed1_net.h) */
#define D1_XI_A 0x1000u                      /* XInput A of the own pad (game user 0) */
static volatile int d1_legacy;               /* LAB `downed revive old`: the 02a375b hold (game word only, 3-D reach, no grace, no button priority) */
static volatile int d1_amask = 1;            /* the revive owns A next to the lying partner (`downed amask 0` = the stock action may take it) */
static int d1_mask_on;                       /* testmode_pad_mask(0, A) is set now */
static volatile uint32_t d1_force_until;     /* LAB `downed hold <ms>` */
typedef struct { int game_a, raw_a, in_range, in_play, owns; float dist, dy; } d1_hold_in;
static d1_hold_in d1_last_in;                /* the inputs of the last pass (the CANCEL line) */
static uint32_t d1_hpwatch_until;            /* after the partner's revive: his hp per his STATE is logged once it is above 1 */
static int d1_hpwatch_done;

static void d1_log(const char *fmt, ...)
{
    if (d1_nlog++ >= D1_LOG_MAX)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r1_log("%s", buf);
}

static unsigned rd32(unsigned a) { unsigned v = 0; r1_safe_read((void *)a, &v, 4); return v; }
static float rdf(unsigned a) { float v = 0.0f; r1_safe_read((void *)a, &v, 4); return v; }
static void wrf(unsigned a, float v)
{
    SIZE_T got = 0;
    WriteProcessMemory(GetCurrentProcess(), (void *)a, &v, sizeof v, &got);
}

static float dist_sq(unsigned a, unsigned b)
{
    float dx = rdf(a + D1_POS_OFF) - rdf(b + D1_POS_OFF);
    float dy = rdf(a + D1_POS_OFF + 4) - rdf(b + D1_POS_OFF + 4);
    float dz = rdf(a + D1_POS_OFF + 8) - rdf(b + D1_POS_OFF + 8);
    return dx * dx + dy * dy + dz * dz;
}

static int d1_is_player(unsigned unit)
{
    char cn[96];
    return unit > 0x10000 && classes_object_name(unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4);
}
/* the player unit with slot field +0xE2C = slot (the sPlayer list, as puppet_game.c) */
static unsigned d1_unit_by_slot(unsigned slot)
{
    unsigned sp = rd32(R1_SPLAYER_PTR);
    if (!sp) return 0;
    unsigned head = rd32(sp + 0x30), endp = rd32(sp + 0x34);
    for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8)) {
        unsigned unit = rd32(head + 0x10);
        if (unit && rd32(unit + 0xE2C) == slot && d1_is_player(unit)) return unit;
    }
    return 0;
}

/* ---- the pose calls (downed1_pose.h), real engine side; logic thread only ---- */
typedef float (TC *d1_recover_fn)(void *vital, float hp);   /* returns hp in ST0: declared float so the caller pops it */
typedef void (TC *d1_ban_fn)(void *unit, int on);
typedef int (TC *d1_state_fn)(void *mgr, int idx, int arg, void *args);
typedef void (TC *d1_play_fn)(void *unit, int motion, float blend, float frame, float speed, int flags);

static int e_rd(void *u, uint32_t a, void *dst, int n) { (void)u; return r1_safe_read((void *)a, dst, (unsigned)n); }
static void e_recover(void *u, uint32_t vital, float hp) { (void)u; volatile float r = ((d1_recover_fn)D1P_FN_RECOVER)((void *)vital, hp); (void)r; }
static void e_ban(void *u, uint32_t unit, int on) { (void)u; ((d1_ban_fn)D1P_FN_BAN)((void *)unit, on ? 1 : 0); }
static int e_state(void *u, uint32_t mgr, int idx) { (void)u; return ((d1_state_fn)D1P_FN_STATE)((void *)mgr, idx, 0, NULL) & 0xFF; }
/* the unit's own slot 0x68 (layer 0 of slot 0x6C), only when it is the checked 0x4BBA80: blend 0, from frame 0, flags 4 (as GetUp's entry) */
static int e_play(void *u, uint32_t unit, int motion, float speed)
{
    (void)u;
    unsigned vt = rd32(unit), fn = vt ? rd32(vt + D1P_VT_PLAY) : 0;
    if (fn != D1P_FN_PLAY)
        return 0;
    ((d1_play_fn)fn)((void *)unit, motion, 0.0f, 0.0f, speed, 4);
    return 1;
}
static const d1p_ops d1_ops = {NULL, e_rd, e_recover, e_ban, e_state, e_play};

static int d1_pose_ready(void)
{
    if (d1_pose_ok < 0) {
        const char *bad = "";
        d1_pose_ok = d1p_check(e_rd, NULL, &bad);
        if (d1_pose_ok)
            r1_log("downed1: pose calls checked (%d code places, %d state table words): knock-down, frozen get-up, input ban available", D1P_NSIG, D1P_NDATA);
        else
            r1_log("downed1: pose OFF: %s differs from the supported build (the rule runs with hp only)", bad);
    }
    return d1_pose_ok == 1;
}

static void d1_unit_line(unsigned u, char *out, int size)
{
    uint32_t v = u ? rd32(u + D1P_VITAL_OFF) : 0;
    uint32_t m = u ? rd32(u + D1P_MGR_OFF) : 0;
    snprintf(out, (size_t)size, "unit %08x hp %.1f/%.0f lower %02x cond(F04) %u mode(F08) %u ban %u E1C %08x vital %08x state %u", u, u ? (double)rdf(u + D1P_HP_OFF) : 0.0,
             u ? (double)rdf(u + D1P_MAX_OFF) : 0.0, m ? rd32(m + D1P_MGR_CUR) : 0xFFu, u ? rd32(u + D1P_COND_OFF) : 0, u ? rd32(u + D1P_MODE_OFF) : 0,
             u ? (rd32(u + D1P_PSTATE_OFF) & D1P_BAN_BIT) != 0 : 0, u ? rd32(u + D1P_FLAGS_OFF) : 0, v, v ? rd32(v + D1P_V_STATE) : 0);
}

/* the stock Damage, from the trampoline: it does not come back through the hook */
static void d1_call_stock_damage(unsigned unit, float dmg)
{
    ((d1_damage_fn)d1_tramp)((void *)unit, dmg);
}

/* ---- the hit hook ---- */
static void TC d1_hook_damage(void *self, float dmg)
{
    unsigned unit = (unsigned)self;
    if (d1_god && unit && dmg > 0.0f) {
        unsigned cls = rd32(unit + 0xE28u), caller = (unsigned)(uintptr_t)__builtin_return_address(0);
        if ((cls & 0xF00F0000u) == 0x80010000u) {   /* a human (player class): no damage while the lab god mode is on */
            InterlockedIncrement(&d1_god_skipped);
            if (InterlockedIncrement(&d1_god_nlog) <= 80)
                r1_log("downed1: LAB GOD: damage %.1f to human %08x (slot %u, hp %.1f) skipped, caller %08x", (double)dmg, unit, rd32(unit + 0xE2Cu), (double)rdf(unit + D1_HP_OFF), caller);
            d1_call_stock_damage(unit, 0.0f);
            return;
        }
        if ((cls & 0xF00F0000u) == 0x80030000u) {   /* an enemy: logged only (the world's HIT applier and the engine's own hits both come here) */
            InterlockedIncrement(&d1_god_enemy);
            if (InterlockedIncrement(&d1_god_nlog) <= 160)
                r1_log("downed1: LAB GOD: damage %.1f to enemy %08x class %08x hp %.1f -> %.1f, caller %08x", (double)dmg, unit, cls, (double)rdf(unit + D1_HP_OFF),
                       (double)(rdf(unit + D1_HP_OFF) - dmg), caller);
        }
    }
    if (!d1_on) {
        d1_call_stock_damage(unit, dmg);
        return;
    }
    if (d1_netmode) {
        if (dmg > 0.0f && unit && unit == d1_net_own) {
            float hp = rdf(unit + D1_HP_OFF);
            EnterCriticalSection(&d1_cs);
            float before = dmg;
            dmg = d1n_absorb(&d1_net, hp, dmg, GetTickCount());
            LeaveCriticalSection(&d1_cs);
            if (dmg != before)
                d1_log("downed1: net: hit on the own human (slot %d): hp %.1f, damage %.1f -> %.1f (kept at 1 hp, the rule)", d1_net_me, hp, before, dmg);
        }
        d1_call_stock_damage(unit, dmg);
        return;
    }
    if (dmg > 0.0f && unit && (unit == d1_units[0] || unit == d1_units[1])) {
        int k = unit == d1_units[0] ? 0 : 1;
        float hp = rdf(unit + D1_HP_OFF);
        EnterCriticalSection(&d1_cs);
        float before = dmg;
        dmg = d1_absorb(&d1_pair, k, hp, dmg, GetTickCount());
        LeaveCriticalSection(&d1_cs);
        if (dmg != before)
            d1_log("downed1: hit on human %d: hp %.1f, damage %.1f -> %.1f (kept at 1 hp, the rule)", k, hp, before, dmg);
    }
    d1_call_stock_damage(unit, dmg);
}

/* the inline hook, installed on the logic thread between two passes (the same rule as capture.c) */
static int d1_install(void)
{
    unsigned char cur[8], patch[8];
    if (!r1_safe_read((void *)D1_DAMAGE_ADDR, cur, sizeof cur) || memcmp(cur, d1_damage_bytes, sizeof cur) != 0) {
        r1_log("downed1: bytes at %08x differ from the supported build, the rule stays off", D1_DAMAGE_ADDR);
        return 0;
    }
    if (!d1_tramp) {
        unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!t)
            return 0;
        memcpy(t, cur, sizeof cur);
        t[8] = 0xE9;
        *(int *)(t + 9) = (int)((D1_DAMAGE_ADDR + 8u) - (unsigned)(t + 13));
        d1_tramp = t;
    }
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    *(int *)(patch + 1) = (int)((unsigned)d1_hook_damage - (D1_DAMAGE_ADDR + 5u));
    DWORD old;
    if (!VirtualProtect((void *)D1_DAMAGE_ADDR, sizeof patch, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(d1_patch_written, patch, sizeof patch);   /* before the write: the world's check may run on another pass right after it */
    memcpy((void *)D1_DAMAGE_ADDR, patch, sizeof patch);
    VirtualProtect((void *)D1_DAMAGE_ADDR, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)D1_DAMAGE_ADDR, sizeof patch);
    d1_hooked = 1;
    r1_log("downed1: Damage hook installed at %08x", D1_DAMAGE_ADDR);
    return 1;
}

/* the actions of the pair, applied to the human's unit (logic thread, outside the lock: the engine calls may come back into the game) */
static void d1_apply(const d1_action *a, unsigned unit)
{
    if (!unit)
        return;
    char line[200];
    switch (a->act) {
    case D1_A_GO_DOWN: {
        wrf(unit + D1_HP_OFF, 1.0f);
        int r = d1_pose_on && d1_pose_ready() ? d1p_down(&d1_ops, unit, &d1_ph[a->who], GetTickCount()) : -1;
        d1_unit_line(unit, line, sizeof line);
        d1_log("downed1: human %d is DOWN (hp 1, bleed-out timer runs), pose %s: %s", a->who, r < 0 ? "off" : r == D1P_OK ? "ban + knock-down" : "REFUSED", line);
        break;
    }
    case D1_A_DIE: {
        float hp = rdf(unit + D1_HP_OFF);
        int r = -1;
        if (d1_pose_on && d1_pose_ready())
            r = d1p_die(&d1_ops, unit, &d1_ph[a->who]);   /* the campaign death state 0x32: its entry kills (dead flag, hp 0, the death motion) */
        else if (hp > 0.0f)
            d1_call_stock_damage(unit, hp);   /* without the pose calls: hp 0 (the engine may regenerate it: stage3.md item 8) */
        d1_unit_line(unit, line, sizeof line);
        d1_log("downed1: human %d ends (bleed-out or game over), hp was %.1f, death %s: %s", a->who, hp, r < 0 ? "by Damage" : r == D1P_OK ? "state 0x32" : "REFUSED", line);
        break;
    }
    case D1_A_GET_UP: {
        int r = -1;
        if (d1_pose_on && d1_pose_ready())
            r = d1p_up(&d1_ops, unit, &d1_ph[a->who], D1_REVIVE_PCT);   /* the frozen get-up plays on; hp via the Raid revive function */
        else {
            float mx = rdf(unit + D1_MAX_OFF);
            if (mx > 1.0f)
                wrf(unit + D1_HP_OFF, D1_REVIVE_PCT * mx);
        }
        d1_unit_line(unit, line, sizeof line);
        d1_log("downed1: human %d is up again (%.0f%% hp), pose %s: %s", a->who, D1_REVIVE_PCT * 100.0f, r < 0 ? "off" : r == D1P_OK ? "get-up + hp" : "REFUSED", line);
        break;
    }
    case D1_A_REVIVE_PLAY:
        d1_log("downed1: human %d is being revived (no reviver pose: the Raid prompt is not used, see docs)", a->who);
        break;
    case D1_A_REVIVE_STOP:
        d1_log("downed1: revive of human %d stopped", a->who);
        break;
    default:
        break;
    }
}

/* the own pad's A back to the game (no session, loading, rule off) */
static void d1_mask_off(void)
{
    if (!d1_mask_on) return;
    testmode_pad_mask(0, 0);
    d1_mask_on = 0;
    d1_log("downed1: net: the revive gives back the A button (no play)");
}

/* events of the other PC arrive on the session's thread (the link's event callback, its lock held): only queued here */
static void d1_on_event(void *user, const cp_event *e)
{
    (void)user;
    if (e->kind != CP_EV_D1_KIND || e->len == 0 || e->len > 16) return;
    EnterCriticalSection(&d1_eq_cs);
    int next = (d1_eq_head + 1) % 32;
    if (next != d1_eq_tail) {
        d1_eq[d1_eq_head].len = (uint8_t)e->len;
        memcpy(d1_eq[d1_eq_head].b, e->body, e->len);
        d1_eq_head = next;
    }
    LeaveCriticalSection(&d1_eq_cs);
}

/* one pass of the network mode (logic thread) */
static void d1_net_frame(lk_t *k, uint32_t now)
{
    lk_info in;
    lk_get_info(k, &in);
    cs_t *cs = lk_session(k);
    if (!cs || in.role < 0 || in.role > 1) return;
    int me = in.role, other = 1 - in.role;
    if (d1_reg_link != k) {
        lk_add_event_cb(k, d1_on_event, NULL);
        d1_reg_link = k;
    }
    if (!d1_netmode || d1_net_me != me) {
        unsigned sf = (unsigned)in.set.flags & 7u;   /* ep1-flow E12: the host's choice from the session record (the same on both PCs) */
        EnterCriticalSection(&d1_cs);
        d1n_init(&d1_net, sf, me, now);
        LeaveCriticalSection(&d1_cs);
        r1_log("downed1: the session's rule (host's choice, flags %u): %s, bleed-out %s", sf, sf & 1u ? "ON" : "off",
               !(sf & 1u) ? "-" : ((sf >> 1) & 3u) == 3u ? "never" : ((sf >> 1) & 3u) == 1u ? "30 s" : ((sf >> 1) & 3u) == 2u ? "120 s" : "60 s");
        memset(d1_ph, 0, sizeof d1_ph);
        d1_net_me = me, d1_net_own = 0, d1_net_pup = 0, d1_net_absent = 0;
        d1_netmode = 1;
        r1_log("downed1: NETWORK mode: this PC's human is slot %d; the partner's rule runs on his PC (events kind 0x%x), his puppet here mirrors DOWN / revive / death", me, CP_EV_D1_KIND);
    }
    if (cs_phase(cs) != CP_PHASE_PLAYING) {   /* loading: nothing to decide, the queued events wait */
        d1_mask_off();
        return;
    }
    unsigned ep = cs_epoch(cs);
    unsigned own = d1_unit_by_slot((unsigned)me), pup = d1_unit_by_slot((unsigned)other);
    EnterCriticalSection(&d1_cs);
    int newep = d1n_epoch(&d1_net, ep, now);
    LeaveCriticalSection(&d1_cs);
    if (newep) {
        memset(d1_ph, 0, sizeof d1_ph);
        d1_log("downed1: net: level instance %u: both humans up", ep);
    }
    if (own != d1_net_own || pup != d1_net_pup) {
        d1_log("downed1: net: own human unit %08x (slot %d), partner's puppet unit %08x (slot %d)", own, me, pup, other);
        if (own != d1_net_own) memset(&d1_ph[me], 0, sizeof d1_ph[me]);
        if (pup != d1_net_pup) memset(&d1_ph[other], 0, sizeof d1_ph[other]);
        d1_net_own = own, d1_net_pup = pup;
    }
    int connected = cs_remote_pid(cs) >= 0 && pup;
    if (connected) d1_net_absent = 0;
    else if (d1_net_absent < 1000u) d1_net_absent++;
    int present = connected || d1_net_absent < 30u;
    {   /* ep1-solo: the story switched the partner off here (1-3 solo part): nobody can revive, so he does not count as present (the stock solo death rule) */
        static int off_seen;
        int off = pup && solo1_unit_off(pup);
        if (off != off_seen) off_seen = off, d1_log("downed1: net: the partner (slot %d) is %s", other, off ? "switched off by the story here: not present (a lethal hit is an ordinary death)" : "back in play: present again");
        present = s1_partner_present(present, off);
    }

    /* the partner's events, then his flags (a lost DOWN / DONE is repaired after 600 ms; DEAD is never taken from the flags) */
    for (;;) {
        d1_evq_t ev;
        EnterCriticalSection(&d1_eq_cs);
        int have = d1_eq_tail != d1_eq_head;
        if (have) ev = d1_eq[d1_eq_tail], d1_eq_tail = (d1_eq_tail + 1) % 32;
        LeaveCriticalSection(&d1_eq_cs);
        if (!have) break;
        EnterCriticalSection(&d1_cs);
        int ok = d1n_recv(&d1_net, ev.b, ev.len, now);
        LeaveCriticalSection(&d1_cs);
        d1_net_got++;
        d1_log("downed1: net: got %s from slot %d (arg %d, epoch %u)%s", d1n_ev_name(ev.len > 2 ? ev.b[2] : 0), ev.len > 3 ? ev.b[3] : -1, ev.len > 4 && ev.b[4] != 0xFF ? ev.b[4] : -1,
               ev.len > 1 ? ev.b[1] : 0, ok ? "" : " - REFUSED (bad / other level instance / not the partner)");
    }
    unsigned pfl = 0;
    int have_fl = puppet_partner_flags(&pfl);

    /* ---- the revive hold (round 4): the button from the game's word OR the own pad (the revive owns A next to the lying partner: the game does not get it, see
     *      downed1_net.h), the reach horizontal + floor with a keep range, a short gap bridged. `downed revive old` = the 02a375b hold (A/B). ---- */
    unsigned gp = rd32(R1_GAMEPAD_PTR);
    capture_state_t st;
    capture_get_state(&st);
    int pad = st.role[me].enabled ? st.role[me].pad : 0;
    unsigned held = gp && own && pad >= 0 && pad < 4 ? rd32(gp + 0x3Cu + 0x2CCu * (unsigned)pad + 0x15Cu) : 0;
    int game_a = (held & D1_BTN_REVIVE) != 0;
    unsigned char xg[16];
    memset(xg, 0, sizeof xg);
    int raw_a = testmode_pad_peek(0, xg) && ((xg[0] | (xg[1] << 8)) & D1_XI_A) != 0;   /* XINPUT_GAMEPAD.wButtons: this PC's own pad (game user 0) before the mask */
    unsigned sf = 0;
    int in_play = puppet_scene_now(&sf) && !(sf & (CP_SF_LOADING | CP_SF_CUTSCENE | CP_SF_PAUSED));
    float dx = 0, dy = 0, dz = 0;
    if (own && pup) dx = rdf(pup + D1_POS_OFF) - rdf(own + D1_POS_OFF), dy = rdf(pup + D1_POS_OFF + 4) - rdf(own + D1_POS_OFF + 4), dz = rdf(pup + D1_POS_OFF + 8) - rdf(own + D1_POS_OFF + 8);
    EnterCriticalSection(&d1_cs);
    d1_net.legacy = d1_legacy;
    int in_range = own && pup && d1n_in_reach(&d1_net, dx, dy, dz);
    int owns = own && pup && in_play && d1_amask && d1n_owns_button(&d1_net, in_range);
    int partner_down = d1n_partner_down(&d1_net), running = d1n_hold_running(&d1_net);
    uint32_t gaps = d1_net.hold_gaps, bridged = d1_net.hold_bridged;
    LeaveCriticalSection(&d1_cs);
    if (owns != d1_mask_on) {
        testmode_pad_mask(0, owns ? D1_XI_A : 0);
        d1_mask_on = owns;
        d1_log("downed1: net: the revive %s the A button (partner %s, distance %.1f h / dy %.1f)", owns ? "OWNS" : "gives back", partner_down ? "DOWN" : "not down", (double)sqrtf(dx * dx + dz * dz), (double)dy);
    }
    int forced = (int32_t)(d1_force_until - now) > 0;   /* LAB `downed hold <ms>`: the button counts as held (input bypass, tells input from range live) */
    EnterCriticalSection(&d1_cs);
    int holding = forced || d1n_button(&d1_net, game_a, raw_a, in_play);
    LeaveCriticalSection(&d1_cs);
    {   /* what the hold sees, when it changes while the partner lies or a hold runs (wp7: the hold ended after 0.73 s) */
        static int last = -1;
        static unsigned nlog;
        int key = holding | in_range << 1 | game_a << 2 | raw_a << 3 | owns << 4 | in_play << 5 | running << 6 | forced << 7;
        if ((partner_down || running) && key != last && nlog++ < 160) {
            unsigned hidden = 0;
            testmode_pad_masked(0, &hidden);
            d1_log("downed1: net: revive hold input: A game %d raw %d%s (held word %08x, pad %d) -> holding %d; scene %x%s; distance %.1f h / dy %.1f -> %s; owns A %d (hidden %u); own lower %02x; "
                   "hold %s (gaps %u bridged %u)%s",
                   game_a, raw_a, forced ? " FORCED" : "", held, pad, holding, sf, in_play ? "" : " (not in play)", (double)sqrtf(dx * dx + dz * dz), (double)dy, in_range ? "in range" : "OUT of range",
                   owns, hidden, own ? rd32(rd32(own + D1P_MGR_OFF) + D1P_MGR_CUR) : 0xFFu, running ? "running" : "idle", gaps, bridged, d1_legacy ? " [LEGACY hold]" : "");
        }
        if (partner_down || running) last = key;
        else last = -1;
    }
    d1_last_in = (d1_hold_in){game_a, raw_a, in_range, in_play, owns, sqrtf(dx * dx + dz * dz), dy};
    d1n_action acts[24];
    EnterCriticalSection(&d1_cs);
    if (have_fl && present) {
        d1n_sample(&d1_net, (pfl & CP_SF_DOWN) != 0, now);
        d1_net_samples++;
    }
    d1n_present(&d1_net, present, now);
    if (own && rdf(own + D1_HP_OFF) <= 0.0f && dl_state(&d1_net.d, me) != DL_DEAD)
        d1n_dead(&d1_net, now);   /* the engine killed him (no absorbed hit): the partner's rule learns it */
    d1n_hold(&d1_net, holding, in_range, now);
    int own_down = own && d1n_own_down(&d1_net);
    if (own_down && rdf(own + D1_HP_OFF) != 1.0f)
        wrf(own + D1_HP_OFF, 1.0f);   /* a DOWN human stays at 1 hp: the regeneration does not raise him */
    int na = d1n_step(&d1_net, now, acts, 24);
    int still_own = own && d1n_own_down(&d1_net), still_pup = pup && d1n_partner_down(&d1_net);
    LeaveCriticalSection(&d1_cs);

    for (int i = 0; i < na; i++) {
        d1n_action *a = &acts[i];
        char line[200];
        switch (a->act) {
        case D1N_A_SEND: {
            int ok = cs_send_event(cs, CP_EV_D1_KIND, a->msg, (unsigned)a->len);
            if (ok) d1_net_sent++;
            else d1_net_send_fail++;
            d1_log("downed1: net: sent %s (arg %d, epoch %u)%s", d1n_ev_name(a->msg[2]), a->msg[4] == 0xFF ? -1 : a->msg[4], a->msg[1], ok ? "" : " - SEND FAILED");
            if (a->msg[2] == DL_E_CANCEL)   /* why the own hold ended (wp7: unknown) */
                d1_log("downed1: net: own hold ended: A game %d raw %d, %s (distance %.1f h / dy %.1f), scene %s, owns A %d, own %s, partner %s", d1_last_in.game_a, d1_last_in.raw_a,
                       d1_last_in.in_range ? "in range" : "OUT of range", (double)d1_last_in.dist, (double)d1_last_in.dy, d1_last_in.in_play ? "play" : "not in play", d1_last_in.owns,
                       dl_state_name(dl_state(&d1_net.d, me)), dl_state_name(dl_state(&d1_net.d, other)));
            break;
        }
        case D1N_A_GO_DOWN: case D1N_A_DIE: case D1N_A_GET_UP: case D1N_A_REVIVE_PLAY: case D1N_A_REVIVE_STOP: case D1N_A_HOLD_RESET: {
            d1_action x;
            x.act = a->act == D1N_A_GO_DOWN ? D1_A_GO_DOWN : a->act == D1N_A_DIE ? D1_A_DIE : a->act == D1N_A_GET_UP ? D1_A_GET_UP
                  : a->act == D1N_A_REVIVE_PLAY ? D1_A_REVIVE_PLAY : a->act == D1N_A_REVIVE_STOP ? D1_A_REVIVE_STOP : D1_A_HOLD_RESET;
            x.who = me, x.arg = a->arg;
            d1_apply(&x, own);
            break;
        }
        case D1N_A_PUP_DOWN: {
            int r = d1_pose_on && d1_pose_ready() && pup ? d1p_down(&d1_ops, pup, &d1_ph[other], now) : -1;
            d1_unit_line(pup, line, sizeof line);
            d1_log("downed1: net: the partner (slot %d) is DOWN on his PC: his puppet %s: %s", other, r < 0 ? "not posed (no pose / no unit)" : r == D1P_OK ? "knocked down" : "REFUSED", line);
            break;
        }
        case D1N_A_PUP_UP: {
            int r = d1_pose_on && d1_pose_ready() && pup ? d1p_up(&d1_ops, pup, &d1_ph[other], D1_REVIVE_PCT) : -1;
            d1_unit_line(pup, line, sizeof line);
            d1_log("downed1: net: the partner (slot %d) is up again: his puppet %s: %s", other, r < 0 ? "not posed" : r == D1P_OK ? "gets up" : "REFUSED", line);
            d1_hpwatch_until = now + 6000u, d1_hpwatch_done = 0;   /* his PC runs the get-up when the DONE arrives: his hp (his STATE) follows */
            break;
        }
        case D1N_A_PUP_DIE: {
            int r = d1_pose_on && d1_pose_ready() && pup ? d1p_die(&d1_ops, pup, &d1_ph[other]) : -1;
            d1_unit_line(pup, line, sizeof line);
            d1_log("downed1: net: the partner (slot %d) is DEAD on his PC: his puppet %s: %s", other, r < 0 ? "not posed" : r == D1P_OK ? "death state 0x32" : "REFUSED", line);
            break;
        }
        case D1N_A_GAME_OVER:
            d1_log("downed1: net: GAME OVER decided on this PC (%s)", a->arg ? "a human is dead" : "nobody up");
            break;
        default:
            break;
        }
    }
    if (!d1_hpwatch_done && (int32_t)(d1_hpwatch_until - now) > 0) {   /* the revived partner's hp as his own PC reports it (STATE): the hp restore seen on this PC */
        int rp = cs_remote_pid(cs);
        cp_state rs;
        if (rp >= 0 && cs_remote_sample(cs, rp, cs_now(cs), &rs) != CS_SAMPLE_NONE && rs.hp > 1) {
            d1_hpwatch_done = 1;
            d1_log("downed1: net: the partner (slot %d) after the revive: hp %d/%d per his STATE (puppet here hp %.1f)", other, (int)rs.hp, (int)rs.hpmax, pup ? (double)rdf(pup + D1_HP_OFF) : -1.0);
        }
    } else if (!d1_hpwatch_done && d1_hpwatch_until) {
        d1_hpwatch_done = 1;
        d1_log("downed1: net: the partner (slot %d) after the revive: no STATE with hp > 1 within 6 s", other);
    }
    if (d1_pose_on && d1_pose_ok == 1) {   /* the poses hold while down: put back what a herb, a cutscene or a reset changed */
        int ks[2] = {me, other};
        unsigned us[2] = {still_own ? own : 0, still_pup ? pup : 0};
        for (int j = 0; j < 2; j++) {
            if (!us[j]) continue;
            int did = d1p_keep(&d1_ops, us[j], &d1_ph[ks[j]], now);
            if (did && ++d1_kept[ks[j]] <= 20) {
                char line[200];
                d1_unit_line(us[j], line, sizeof line);
                d1_log("downed1: net: %s (slot %d) pose:%s%s%s: %s", j ? "puppet" : "own human", ks[j], did & 1 ? " landed, lying (get-up frozen)" : "", did & 2 ? " put back lying" : "", did & 4 ? " ban again" : "", line);
            }
        }
    }
}

/* the own human lies (network mode): puppet_game.c adds CP_SF_DOWN to this PC's flags */
int downed1_own_down(void)
{
    if (!d1_netmode) return 0;
    EnterCriticalSection(&d1_cs);
    int r = d1n_own_down(&d1_net);
    LeaveCriticalSection(&d1_cs);
    return r;
}

/* ep1-flow E12: the rule of a network session is the HOST's choice (the lobby row, default on with 60 s): the session record's flags byte, the same on both PCs */
static unsigned d1_session_flags(lk_t *k)
{
    lk_info in;
    lk_get_info(k, &in);
    return (unsigned)in.set.flags & 7u;
}

/* per logic pass */
static void d1_frame(void)
{
    if (!d1_on && !d1_god)
        return;
    /* ep1-flow E12 (product default): ON in a network session whose host has the rule on; the local split rule stays REV1_DOWNED=1 (d1_local). The Damage hook
     * goes in only when something uses it: a solo game is not patched */
    lk_t *k0 = net1_link_peek();
    int net_on = k0 && net1_session_active(), net_rule = d1_on && net_on && (d1_session_flags(k0) & CP_SESS_DOWNED);
    if (!d1_hooked && !d1_hook_failed && (d1_god || d1_local || net_rule) && !d1_install())
        d1_hook_failed = 1;   /* the bytes differ or the memory is not writable: the rule stays off for this run */
    if (!d1_on)
        return;               /* only the lab god mode: the hook is in, no rule */
    if (d1_pose_on && d1_pose_ok < 0 && (d1_local || net_rule))
        d1_pose_ready();      /* once: the pose calls checked by bytes (refused = the rule without the pose) */
    if (d1_hook_failed)
        return;
    {
        lk_t *k = k0;
        if (net_on) {
            if (net_rule && d1_hooked) d1_net_frame(k, GetTickCount());   /* the host chose Off: nothing here */
            return;
        }
        if (d1_netmode) {   /* the session ended: back to the local rule (split / solo) */
            d1_mask_off();
            d1_netmode = 0, d1_net_own = 0, d1_net_pup = 0, d1_net_me = -1;
            for (int k2 = 0; k2 < D1_N; k2++) d1_units[k2] = 0;
            r1_log("downed1: network session over: back to the local rule");
        }
    }
    if (!d1_local)
        return;   /* ep1-flow: the local split rule is the lab's (REV1_DOWNED=1), as before */
    capture_state_t st;
    capture_get_state(&st);
    unsigned units[D1_N] = {0, 0};
    int pads[D1_N] = {0, 0};
    if (st.game_mode == 0) {
        for (int k = 0; k < D1_N; k++)
            if (st.role[k].enabled && st.role[k].unit > 0x10000u)
                units[k] = st.role[k].unit, pads[k] = st.role[k].pad;
    }
    uint32_t now = GetTickCount();
    unsigned gp = rd32(R1_GAMEPAD_PTR);
    d1_action acts[16];
    int nacts = 0;

    /* presence is debounced: a human counts as gone only after 30 passes without a unit (a role that flickers during a swap is not a leave) */
    int present[D1_N];
    for (int k = 0; k < D1_N; k++) {
        if (units[k])
            d1_absent[k] = 0;
        else if (d1_absent[k] < 1000u)
            d1_absent[k]++;
        present[k] = units[k] != 0 || d1_absent[k] < 30u;
    }

    EnterCriticalSection(&d1_cs);
    int reset = 0;   /* a load / checkpoint / area change recreates a human's unit: the pair starts again */
    for (int k = 0; k < D1_N; k++) {
        if (units[k] && units[k] != d1_units[k]) {
            d1_units[k] = units[k];
            reset = 1;
            d1_log("downed1: human %d is unit %08x (pad %d)", k, units[k], pads[k]);
        }
    }
    if (reset) {
        d1_init(&d1_pair, d1_flags, now);
        memset(d1_ph, 0, sizeof d1_ph);
        d1_log("downed1: state reset");
    }
    for (int k = 0; k < D1_N; k++)
        d1_set_present(&d1_pair, k, present[k], now);
    for (int k = 0; k < D1_N; k++) {
        unsigned u = units[k];
        if (!u)
            continue;
        if (rdf(u + D1_HP_OFF) <= 0.0f)   /* the engine killed him (no absorbed hit): the other PC's rule learns it */
            d1_dead(&d1_pair, k, now);
        unsigned other = units[1 - k];
        int in_range = other && dist_sq(u, other) <= D1_REACH * D1_REACH;
        int holding = 0;
        if (gp && pads[k] >= 0 && pads[k] < 4)
            holding = (rd32(gp + 0x3Cu + 0x2CCu * (unsigned)pads[k] + 0x15Cu) & D1_BTN_REVIVE) != 0;
        d1_hold(&d1_pair, k, holding, in_range, now);
    }
    int down[D1_N] = {0, 0};
    for (int k = 0; k < D1_N; k++) {   /* a DOWN human stays at 1 hp: the regeneration does not raise him */
        down[k] = units[k] && d1_is_down(&d1_pair, k);
        if (down[k] && rdf(units[k] + D1_HP_OFF) != 1.0f)
            wrf(units[k] + D1_HP_OFF, 1.0f);
    }
    nacts = d1_step(&d1_pair, now, acts, 16);
    int still[D1_N];
    for (int k = 0; k < D1_N; k++)
        still[k] = down[k] && d1_is_down(&d1_pair, k);
    LeaveCriticalSection(&d1_cs);

    for (int i = 0; i < nacts; i++)
        d1_apply(&acts[i], units[acts[i].who]);
    if (d1_pose_on && d1_pose_ok == 1)   /* the pose holds while he is down: put back what a herb, a cutscene or a reset changed */
        for (int k = 0; k < D1_N; k++) {
            if (!still[k])
                continue;
            int did = d1p_keep(&d1_ops, units[k], &d1_ph[k], now);
            if (did && ++d1_kept[k] <= 20) {
                char line[200];
                d1_unit_line(units[k], line, sizeof line);
                d1_log("downed1: human %d pose:%s%s%s: %s", k, did & 1 ? " landed, lying (get-up frozen)" : "", did & 2 ? " put back lying" : "", did & 4 ? " ban again" : "", line);
            }
        }
}

/* console (logic thread): downed [status] | down <k> | pose <k> down|keep|up|die (lab: the pose alone, no rule) */
void downed_cmd(const char *arg, char *out, int size)
{
    char w[16] = "", w2[16] = "";
    int k = -1;
    sscanf(arg, "%15s %d %15s", w, &k, w2);
    capture_state_t st;
    capture_get_state(&st);
    unsigned u = (k == 0 || k == 1) && st.role[k].enabled ? st.role[k].unit : 0;
    if (!strcmp(w, "god")) {   /* LAB ONLY: downed god 0|1 */
        if (k == 0 || k == 1) d1_god = k;
        snprintf(out, (size_t)size, "downed1: LAB GOD %s (humans take no damage; skipped %ld, enemy damage lines %ld); Damage hook %s", d1_god ? "ON" : "off", (long)d1_god_skipped,
                 (long)d1_god_enemy, d1_hooked ? "in" : d1_hook_failed ? "REFUSED (bytes)" : "not yet (next pass)");
        return;
    }
    if (!strcmp(w, "revive")) {   /* LAB A/B: downed revive old|new (old = the 02a375b hold: game word only, 3-D reach 170, no grace, no button priority) */
        char v[16] = "";
        sscanf(arg, "%*s %15s", v);
        if (!strcmp(v, "old")) d1_legacy = 1;
        else if (!strcmp(v, "new")) d1_legacy = 0;
        snprintf(out, (size_t)size, "downed1: revive hold %s (button priority %s)", d1_legacy ? "OLD (02a375b: game word, 3-D reach, no grace)" : "new (game word or own pad, reach h + floor + keep, grace)",
                 d1_amask && !d1_legacy ? "on" : "off");
        return;
    }
    if (!strcmp(w, "amask")) {   /* downed amask 0|1: the revive owns A next to the lying partner (default 1) */
        if (k == 0 || k == 1) d1_amask = k;
        unsigned hidden = 0, m = testmode_pad_masked(0, &hidden);
        snprintf(out, (size_t)size, "downed1: button priority %s; mask of game user 0 now %04x, polls with A hidden %u", d1_amask ? "ON" : "off", m, hidden);
        return;
    }
    if (!strcmp(w, "hold")) {   /* LAB: downed hold <ms>: the revive button counts as held for ms (tells the input from the range live) */
        d1_force_until = GetTickCount() + (uint32_t)(k > 0 && k < 20000 ? k : 0);
        snprintf(out, (size_t)size, "downed1: revive button forced for %d ms", k > 0 && k < 20000 ? k : 0);
        return;
    }
    if (!w[0] || !strcmp(w, "status")) {
        int o = snprintf(out, (size_t)size, "downed1: rule %s, pose %s (%s), flags %u, LAB GOD %s (skipped %ld);", d1_on ? "ON" : "off", d1_pose_on ? "on" : "off",
                         d1_pose_ok < 0 ? "not checked" : d1_pose_ok ? "bytes ok" : "BYTES DIFFER", d1_flags, d1_god ? "ON" : "off", (long)d1_god_skipped);
        {
            unsigned hidden = 0, m = testmode_pad_masked(0, &hidden);
            o += snprintf(out + o, (size_t)(size - o), " revive hold %s, button priority %s (mask %04x, hidden %u), hold gaps %u bridged %u cancels %u;", d1_legacy ? "OLD" : "new",
                          d1_amask ? "on" : "off", m, hidden, d1_net.hold_gaps, d1_net.hold_bridged, d1_net.hold_cancels);
        }
        for (int i = 0; i < D1_N && o < size - 200; i++) {
            char line[200];
            d1_unit_line(st.role[i].enabled ? st.role[i].unit : 0, line, sizeof line);
            o += snprintf(out + o, (size_t)(size - o), " | human %d: %s", i, line);
        }
        if (d1_on && o < size - 300) {
            EnterCriticalSection(&d1_cs);
            o += snprintf(out + o, (size_t)(size - o), " | ");
            if (d1_netmode) {
                o += snprintf(out + o, (size_t)(size - o), "NETWORK mode, me = slot %d, own %08x, puppet %08x; events sent %u (failed %u), got %u (applied %u, stale %u, bad %u, foreign %u), flag samples %u | ",
                              d1_net_me, d1_net_own, d1_net_pup, d1_net_sent, d1_net_send_fail, d1_net_got, d1_net.rx, d1_net.rx_stale, d1_net.rx_bad, d1_net.rx_foreign, d1_net_samples);
                dl_status(&d1_net.d, out + o, size - o);
            } else {
                dl_status(&d1_pair.d[0], out + o, size - o);
            }
            LeaveCriticalSection(&d1_cs);
        }
        return;
    }
    if (!u && !(d1_netmode && !strcmp(w, "down"))) {
        snprintf(out, (size_t)size, "usage: downed [status] | down <0|1> | pose <0|1> down|keep|up|die  (human k must be captured)");
        return;
    }
    if (!strcmp(w, "down")) {   /* a lethal hit as the rule sees it, without an enemy: the next pass puts him DOWN */
        if (!d1_on) { snprintf(out, (size_t)size, "the rule is off (REV1_DOWNED=1)"); return; }
        if (d1_netmode) {   /* network: only this PC's own human (the partner's rule is on his PC) */
            if (k != d1_net_me || !d1_net_own) { snprintf(out, (size_t)size, "network mode: only the own human (slot %d) can be put down here", d1_net_me); return; }
            u = d1_net_own;
            float hp = rdf(u + D1_HP_OFF);
            EnterCriticalSection(&d1_cs);
            float d = d1n_absorb(&d1_net, hp, hp + 1000.0f, GetTickCount());
            LeaveCriticalSection(&d1_cs);
            if (d < hp + 1000.0f) {
                wrf(u + D1_HP_OFF, hp - d > 1.0f ? hp - d : 1.0f);
                snprintf(out, (size_t)size, "human %d (own, network): lethal hit absorbed (hp %.1f -> 1), DOWN on the next pass", k, hp);
            } else {
                snprintf(out, (size_t)size, "human %d (own, network): the rule did not absorb it (nobody up to raise him, or dead): nothing done", k);
            }
            return;
        }
        float hp = rdf(u + D1_HP_OFF);
        EnterCriticalSection(&d1_cs);
        float d = d1_absorb(&d1_pair, k, hp, hp + 1000.0f, GetTickCount());
        LeaveCriticalSection(&d1_cs);
        if (d < hp + 1000.0f) {
            wrf(u + D1_HP_OFF, hp - d > 1.0f ? hp - d : 1.0f);
            snprintf(out, (size_t)size, "human %d: lethal hit absorbed (hp %.1f -> 1), DOWN on the next pass", k, hp);
        } else {
            snprintf(out, (size_t)size, "human %d: the rule did not absorb it (nobody up to raise him, or dead): nothing done", k);
        }
        return;
    }
    if (!strcmp(w, "pose")) {
        if (!d1_pose_ready()) { snprintf(out, (size_t)size, "pose calls not available (bytes differ)"); return; }
        uint32_t now = GetTickCount();
        int r = !strcmp(w2, "down") ? d1p_down(&d1_ops, u, &d1_ph[k], now) : !strcmp(w2, "keep") ? d1p_keep(&d1_ops, u, &d1_ph[k], now)
              : !strcmp(w2, "up") ? d1p_up(&d1_ops, u, &d1_ph[k], D1_REVIVE_PCT) : !strcmp(w2, "die") ? d1p_die(&d1_ops, u, &d1_ph[k]) : -1;
        char line[200];
        d1_unit_line(u, line, sizeof line);
        snprintf(out, (size_t)size, "pose %s on human %d -> %d (phase %d, frozen %02x): %s", w2, k, r, d1_ph[k].phase, d1_ph[k].frozen, line);
        return;
    }
    snprintf(out, (size_t)size, "unknown: %s", w);
}

int downed1_init(void)
{
    if (d1_inited)
        return d1_on;
    d1_inited = 1;
    InitializeCriticalSection(&d1_cs);
    InitializeCriticalSection(&d1_eq_cs);
    char v[8] = "";
    /* ep1-flow E12 (product default): the rule is ON for network sessions (the host's lobby row decides, default 60 s); REV1_DOWNED=0 switches the module off,
     * REV1_DOWNED=1 also runs the local split rule (lab, as before) */
    d1_on = !(r1_env("REV1_DOWNED", v, sizeof v) && v[0] == '0');
    d1_local = r1_env("REV1_DOWNED", v, sizeof v) && v[0] == '1';
    if (r1_env("REV1_DOWNED_POSE", v, sizeof v) && v[0] == '0')
        d1_pose_on = 0;
    if (r1_env("REV1_DOWNED_BLEED", v, sizeof v)) {   /* seconds: 60 (default), 30, 120, 0 = never (the presets of the session flags) */
        int b = atoi(v);
        d1_flags = CP_SESS_DOWNED | ((b == 30 ? 1u : b == 120 ? 2u : b == 0 && v[0] == '0' ? 3u : 0u) << 1);
    }
    if (r1_env("REV1_LAB_GOD", v, sizeof v) && v[0] == '1')
        d1_god = 1;
    lt_add_frame_cb(d1_frame);   /* always: `downed god 1` may switch the lab god mode on later (the frame returns at once while both are off) */
    if (d1_god)
        r1_log("downed1: LAB GOD ON (REV1_LAB_GOD=1): humans take no damage, damage values logged (test aid)");
    if (!d1_on) {
        r1_log("downed1: off (REV1_DOWNED=0): no downed / revive rule, also not in a network session");
        return 0;
    }
    d1_init(&d1_pair, d1_flags, GetTickCount());
    if (d1_local)
        r1_log("downed1: ON (REV1_DOWNED=1): a hit that would kill goes DOWN while the partner is up (bleed-out %u s, revive by holding A within %.0f units, %.0f%% hp), pose %s; "
               "in a network session the host's session flags decide",
               dl_preset_ms((d1_flags >> 1) & 3u) / 1000u, (double)D1_REACH, (double)(D1_REVIVE_PCT * 100.0f), d1_pose_on ? "knock-down + frozen get-up + input ban" : "off (REV1_DOWNED_POSE=0)");
    else
        r1_log("downed1: ON for network sessions (the host's Downed / revive row, default 60 s; the Damage hook goes in when such a session starts), pose %s; local split rule off "
               "(REV1_DOWNED=1)", d1_pose_on ? "knock-down + frozen get-up + input ban" : "off (REV1_DOWNED_POSE=0)");
    return 1;
}

/* the 8 bytes this module wrote at Damage 0x6CA830 (jmp hook; nop x3), for the world hooks' call-site check; 0 while the hook is not in */
int downed1_damage_patch(unsigned char out[8])
{
    if (!d1_hooked)
        return 0;
    memcpy(out, d1_patch_written, 8);
    return 1;
}
