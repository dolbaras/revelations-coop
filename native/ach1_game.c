/*
 * ach1_game.c: B10, the Steam achievements of each player in a network co-op game (Revelations 1). The decisions are ach1.h (pure, tests\test_ach1.c);
 * this file puts them in front of the game's own profile setters. Model: the Revelations 2 mod's ach.inc (the mod never calls SetAchievement: every
 * unlock still goes through the game's own path on the PC of the human who earned it; the mod only keeps the partner's actions out of this profile).
 *
 * How the game counts (notes\ach-episode1.md, static, build 13298487): cAchievementInfo polls LOCAL state only; the counters of ids 14, 17, 23 and 24
 * live in the profile object [0x116B3A4] and are written by four fastcall(profile) setters, called by game code that does not know about the partner:
 *   C70 0x499260 (+0x70, id 23 ladders)  from ClimbLadder 0x685E60          the climbing unit in ESI at the call 0x685F3C
 *   C71 0x499320 (+0x71, id 24 tech)     from MoveActionParam_vf17 0x6871B0  the unit in EDI at 0x687294
 *                                         from MoveActionParam_vf18 0x6873B0  the unit in ESI at 0x687418
 *                                         from uWeaponExplode_vf39 0x5FD0B0   the weapon's owner unit in ESI at 0x5FD274
 *   C72 0x4993E0 (+0x72, id 14 kills)    from the rank hook 0x899CA0 (uEnemyBase_vf43): HitInfo = its 2nd argument, [esp+0x10] at the call 0x899D01
 *   C74 0x499530 (+0x74, id 17 zombies)  from uZombieBase hit 0x5B8530: HitInfo in EDI at 0x5B87C9
 * In a network game the partner's human is a unit of THIS game too (his puppet, driven by his pad record), so his ladder, his tech actions, his blows on
 * the host's enemies (PCHIT: the host applies them with his puppet as the attacker) would move this profile. The gate: each of these call sites calls a
 * stub instead of the setter (the 5-byte call's target is rewritten after the bytes are checked); the stub asks ach1_gate() who acted:
 *   the own human (player class 0x80010000, unit+0xE2C == the own slot)   -> the setter runs (credited here)
 *   the partner's human (another slot, not the AI kind 2)                   -> skipped: his PC credits his own action (no foreign boost)
 *   the game's AI partner (kind 2), no human (a trap)                        -> kills count as in single player, actor-bound counters do not
 * Outside a started network session (net1_session_active) the stubs pass every call to the setter: single player and split screen stay the game's.
 * Not gated (the actor is not at hand at the call; the partner can rarely reach them): the Genesis scan mask (0x4996D0, id 12), the +0x3C / +0x44 bits
 * (ids 15, 18-21). Save flags (ids 9-11, 16, 22) and episode clears (ids 1-8) are reached by every PC through its own synced world.
 * Lab: REV1_ACHGATE=0 leaves the call sites alone; console `ach` (status | gate 0|1 | taint 0|1 | profile).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "r1.h"
#include "net1.h"
#include "capture.h"
#include "ach1.h"
#include "ach1_game.h"

#define A1G_PROFILE_PTR 0x116B3A4u
#define A1G_PLAYER_CLASS 0x80010000u
#define A1G_U_CLASS 0xE28u
#define A1G_U_SLOT 0xE2Cu
#define A1G_U_KIND 0xE30u

#define A1G_N ACH1_NSITES   /* the call sites: ach1_sites in ach1.h (checked against the image by tests/test_ach1.c) */

/* the stubs below jump to these (the setter of each site) and call ach1g_pass */
uint32_t ach1g_orig[A1G_N];
int __cdecl ach1g_pass(int site, uint32_t src);

/* One stub per site (naked: the setter is fastcall(ECX = profile) with no stack argument and returns its counter in AL; the actor is read from the
 * register or the caller's frame as it is at the call). ach1g_pass answers 1 = run the setter (tail jump, the caller's return address untouched),
 * 0 = skip (AL = the counter as it is, which the callers ignore anyway). */
#define A1G_STUB(n, push_src, off)                                                                                                                         \
    __attribute__((naked, used)) void ach1g_stub##n(void)                                                                                             \
    {                                                                                                                                                      \
        __asm__ volatile("pushl %ecx\n\t" push_src "\n\tpushl $" #n "\n\tcall _ach1g_pass\n\taddl $8, %esp\n\tpopl %ecx\n\ttestl %eax, %eax\n\t"      \
                         "jz 1f\n\tjmp *_ach1g_orig+4*" #n "\n1:\n\tmovzbl " #off "(%ecx), %eax\n\tret");                                                  \
    }
/* (after `pushl %ecx` the caller's [esp+0xc] of the rank hook is at [esp+0x14]: + 4 the return address, + 4 the pushed ECX) */
A1G_STUB(0, "pushl %esi", 0x70)
A1G_STUB(1, "pushl %edi", 0x71)
A1G_STUB(2, "pushl %esi", 0x71)
A1G_STUB(3, "pushl %esi", 0x71)
A1G_STUB(4, "pushl 0x14(%esp)", 0x72)
A1G_STUB(5, "pushl %edi", 0x74)
static void (*const k_stubs[A1G_N])(void) = {ach1g_stub0, ach1g_stub1, ach1g_stub2, ach1g_stub3, ach1g_stub4, ach1g_stub5};

static CRITICAL_SECTION g_lock;
static ach1_state_t g_state;
static ach1_cfg_t g_cfg = {0, 1, 0};
static int g_installed[A1G_N], g_inited, g_any;
static volatile LONG g_calls[A1G_N], g_credited[A1G_N], g_skipped[A1G_N], g_logs;
static int g_was_session;

static uint32_t rd32(uint32_t a)
{
    uint32_t v = 0;
    r1_safe_read((const void *)(uintptr_t)a, &v, 4);
    return v;
}

/* who acted: a player-class unit of this PC's own slot (LOCAL), another player (REMOTE: the partner's puppet), the AI partner (kind 2), or no player */
static int actor_of(uint32_t unit, int own, int *slot, int *kind)
{
    *slot = -1, *kind = -1;
    if (!unit || rd32(unit + A1G_U_CLASS) != A1G_PLAYER_CLASS) return A1A_NONE;
    *slot = (int)rd32(unit + A1G_U_SLOT), *kind = (int)rd32(unit + A1G_U_KIND);
    return ach1_actor_class(*slot, *kind, own);
}

int __cdecl ach1g_pass(int site, uint32_t src)
{
    if (site < 0 || site >= A1G_N) return 1;
    InterlockedIncrement(&g_calls[site]);
    int session = net1_session_active();
    if (!session) {
        InterlockedIncrement(&g_credited[site]);
        return 1;
    }
    const ach1_site_t *s = &ach1_sites[site];
    uint32_t unit = src;
    if (s->src == A1C_HIT_EDI || s->src == A1C_HIT_STACK) {   /* HitInfo: the attacker +0x40, its owner +0x44 (a grenade's thrower) */
        uint32_t a = rd32(src + 0x40), o = rd32(src + 0x44);
        unit = a && rd32(a + A1G_U_CLASS) == A1G_PLAYER_CLASS ? a : o;
    }
    int own = capture_own_effective(), slot, kind;
    int actor = actor_of(unit, own, &slot, &kind);
    EnterCriticalSection(&g_lock);
    if (!g_was_session) {   /* a new session: a new ledger */
        ach1_state_init(&g_state);
        g_was_session = 1;
    }
    g_cfg.session = 1;
    int r = ach1_gate(&g_cfg, &g_state, s->sid, actor, 0, GetTickCount());
    LeaveCriticalSection(&g_lock);
    InterlockedIncrement(r == A1G_ALLOW ? &g_credited[site] : &g_skipped[site]);
    if (InterlockedIncrement(&g_logs) <= 200)
        r1_log("ach: %s by %s (unit %08x slot %d kind %d, own slot %d): %s", s->what, ach1_actor_name(actor), (unsigned)unit, slot, kind, own, ach1_result_name(r));
    return r == A1G_ALLOW;
}

static int patch_call(uint32_t site, uint32_t from_target, void *to)
{
    uint8_t b[5];
    if (!r1_safe_read((const void *)(uintptr_t)site, b, 5) || b[0] != 0xE8) return 0;
    int32_t rel;
    memcpy(&rel, b + 1, 4);
    if (site + 5 + (uint32_t)rel != from_target) return 0;
    int32_t nrel = (int32_t)((uint32_t)(uintptr_t)to - (site + 5));
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    InterlockedExchange((volatile LONG *)(uintptr_t)(site + 1), (LONG)nrel);   /* one aligned-or-not 4-byte store; the call opcode stays */
    VirtualProtect((void *)(uintptr_t)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)site, 5);
    return 1;
}

/* the bytes before each call: `mov ecx, [0x116B3A4]` (8B 0D A4 B3 16 01), the profile the setter gets */
static int site_ok(const ach1_site_t *s)
{
    static const uint8_t mov_ecx[6] = {0x8B, 0x0D, 0xA4, 0xB3, 0x16, 0x01};
    uint8_t b[6];
    return r1_safe_read((const void *)(uintptr_t)(s->site - 6), b, 6) && !memcmp(b, mov_ecx, 6);
}

void ach1_game_init(void)
{
    char v[8];
    if (g_inited) return;
    g_inited = 1;
    InitializeCriticalSection(&g_lock);
    ach1_state_init(&g_state);
    if (r1_env("REV1_ACHGATE", v, sizeof v) && v[0] == '0') {
        r1_log("ach: REV1_ACHGATE=0: the achievement gate is not installed (the partner's actions would count on this profile in a network game)");
        return;
    }
    int n = 0;
    for (int i = 0; i < A1G_N; i++) {
        const ach1_site_t *s = &ach1_sites[i];
        ach1g_orig[i] = s->setter;
        if (!site_ok(s) || !patch_call(s->site, s->setter, (void *)k_stubs[i])) {
            r1_log("ach: site %08x (%s): bytes differ, NOT gated", (unsigned)s->site, s->what);
            continue;
        }
        g_installed[i] = 1, n++;
    }
    g_any = n > 0;
    r1_log("ach: gate in front of %d of %d profile setter calls (ids 14, 17, 23, 24: only this PC's human is credited in a network game)", n, A1G_N);
}

/* the frame: after a session ends the next one starts with a clean ledger */
void ach1_game_frame(void)
{
    if (g_was_session && !net1_session_active()) {
        EnterCriticalSection(&g_lock);
        g_was_session = 0;
        g_cfg.session = 0;
        LeaveCriticalSection(&g_lock);
    }
}

void ach1_game_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "";
    int val = -1;
    sscanf(arg ? arg : "", "%15s %d", sub, &val);
    if (!strcmp(sub, "gate") && val >= 0) {
        g_cfg.gate_on = val != 0;
        snprintf(out, (size_t)size, "ach: gate %s", g_cfg.gate_on ? "on" : "off (the game's own calls stand)");
        return;
    }
    if (!strcmp(sub, "taint") && val >= 0) {
        g_cfg.tainted = val != 0;
        snprintf(out, (size_t)size, "ach: taint %d (1 = nothing is credited in a session)", g_cfg.tainted);
        return;
    }
    if (!strcmp(sub, "profile")) {
        uint32_t p = rd32(A1G_PROFILE_PTR);
        uint8_t c[8] = {0};
        if (p) r1_safe_read((const void *)(uintptr_t)(p + 0x70), c, 5);
        snprintf(out, (size_t)size, "ach: profile %08x: +0x70 ladders %u, +0x71 tech %u, +0x72 kills %u, +0x74 zombie hits %u, +0x3C %08x, +0x44 %08x, +0x6C scans %08x",
                 (unsigned)p, c[0], c[1], c[2], c[4], p ? (unsigned)rd32(p + 0x3C) : 0, p ? (unsigned)rd32(p + 0x44) : 0, p ? (unsigned)rd32(p + 0x6C) : 0);
        return;
    }
    size_t o = (size_t)snprintf(out, (size_t)size, "ach: gate %s%s, session %d, own slot %d", g_any ? (g_cfg.gate_on ? "on" : "off") : "NOT installed",
                                g_cfg.tainted ? " (tainted)" : "", net1_session_active(), capture_own_effective());
    for (int i = 0; i < A1G_N && o + 120 < (size_t)size; i++)
        o += (size_t)snprintf(out + o, (size_t)size - o, "\n  %08x %-48s %s calls %ld credited %ld skipped %ld", ach1_sites[i].site, ach1_sites[i].what,
                              g_installed[i] ? "gated" : "OFF  ", g_calls[i], g_credited[i], g_skipped[i]);
    if (o + 80 < (size_t)size)
        snprintf(out + o, (size_t)size - o, "\n  refused: partner %u, no actor %u, rate %u, section %u, taint %u", g_state.refused[A1G_UNDO_REMOTE], g_state.refused[A1G_UNDO_NOACTOR],
                 g_state.refused[A1G_UNDO_RATE], g_state.refused[A1G_UNDO_SECT], g_state.refused[A1G_UNDO_TAINT]);
}

/* The client's own kill in a network game: the host's DEATH kills the replica through OnHit with an attacker-less HitInfo (world1_game.c op_kill), so the
 * rank hook 0x899CA0 returns before its counter call and the kill is credited by the kill record alone (credit_kill): the client's +0x72 (id 14) never
 * moves for its own kills. This runs the game's own setter for it (through the gate as this PC's human: rate and section limits apply). Game thread.
 * Wired: NOT YET (one call after the kill record in world1_game.c credit_kill; that file is the world agent's: see notes/rel010.md). */
int ach1_game_own_kill(void)
{
    if (!g_any || !net1_session_active()) return 0;
    uint32_t prof = rd32(A1G_PROFILE_PTR);
    if (!prof) return 0;
    EnterCriticalSection(&g_lock);
    g_cfg.session = 1;
    int r = ach1_gate(&g_cfg, &g_state, A1S_C72, A1A_LOCAL, 0, GetTickCount());
    LeaveCriticalSection(&g_lock);
    if (InterlockedIncrement(&g_logs) <= 200) r1_log("ach: own kill credited by the kill record (client): %s", ach1_result_name(r));
    if (r != A1G_ALLOW) return 0;
    ((unsigned char(__attribute__((fastcall)) *)(void *))(uintptr_t)0x4993E0u)((void *)(uintptr_t)prof);
    return 1;
}
