/*
 * capture.c - the second human of Revelations 1 (see capture.h for the model and the API).
 *
 * Findings it is built on (notes\stage3.md, proven live on build 13298487):
 *   - every pad reader of the game indexes by the CURRENT slot sGamePad+0x870 (0 in play); PadData[k] is built by FUN_0088A470 from the raw pad of the current
 *     slot, so the builder is wrapped to build PadData[k] from pad k (padfix),
 *   - everything a character does in its frame is called from the sUnit group loop through three shared uCharaBase phase dispatchers (vtable slots 8, 9, 10 =
 *     0x6CCAB0, 0x6C9C30, 0x6C9C10); wrapping them with "+0x870 = the character's pad slot" covers the character, its weapon units (owner = unit+0x30) and
 *     everything they call, without knowing which function reads which button,
 *   - the upper-body state machine (aim, fire, reload, melee, weapon change) is ticked by a uCallThink unit of its own (phase 0x6C9800, uCallThink+0x30 = the
 *     machine's owner object), wrapped the same way; the machine also has a control mode of its own ([uCallThink+0x34]: 1 human, 2 AI) that must be 1,
 *   - control type [[unit+0xE80]+0x38]+0x34 and kind unit+0xE30 are 1 for a human,
 *   - the own slot FUN_0087E1B0 decides who the engine creates as the human, whose HUD is drawn and whose doors react; the camera target is
 *     [[camroot]+0xCE0]+0x74/+0x78.
 * No Raid code is touched: the module refuses to run (and removes its hooks) while the game mode is not 0 or a Raid session is on.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"
#include "capture.h"
#include "capkeep1.h"
#include "kbskip1.h"
#include "aimgate1.h"

#define TC __attribute__((thiscall))
#define FC __attribute__((fastcall))

static capture_addr_t AD = {0x11E0388u, 0x1169394u, 0x1169108u, 0x1169398u, 0x11E00B8u, 0x88A470u, 0x6CCAB0u, 0x6C9C30u, 0x6C9C10u, 0x6C9800u, 0x87E1B0u, 0x116C3E4u};

void capture_test_set_addrs(const capture_addr_t *a) { AD = *a; }

/* ---- memory helpers ---- */
static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((void *)a, &v, 4);
    return v;
}
static int wr(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)a, p, n, &got) && got == n;
}
static int wr32(unsigned a, unsigned v) { return wr(a, &v, 4); }
static unsigned gp(void) { return *(volatile unsigned *)AD.gamepad_ptr; }
static unsigned ctl_obj_of(unsigned unit) { return rd32(rd32(unit + 0xE80) + 0x38); }

static int is_player_class(unsigned unit)
{
    char cn[96];
    return unit > 0x10000 && classes_object_name(unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4);
}

/* ---- inline hooks (installed and removed on the game thread, whole patch written once) ---- */
typedef struct {
    unsigned target;
    int n;
    unsigned char orig[16];
    void *tramp;
    int on;
} hk_t;

static int hook_install(hk_t *h, unsigned target, const unsigned char *expect, int n, void *hook)
{
    unsigned char cur[16], patch[16];
    if (!r1_safe_read((void *)target, cur, (unsigned)n) || memcmp(cur, expect, (size_t)n) != 0) {
        r1_log("capture: bytes at %08x differ from the supported build, not patching", target);
        return 0;
    }
    if (!h->tramp) {   /* one trampoline per hook for the life of the process (removing and re-adding reuses it) */
        h->tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!h->tramp)
            return 0;
        unsigned char *t = (unsigned char *)h->tramp;
        memcpy(t, cur, (size_t)n);
        t[n] = 0xE9;
        *(int *)(t + n + 1) = (int)((target + (unsigned)n) - (unsigned)(t + n + 5));
    }
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    *(int *)(patch + 1) = (int)((unsigned)hook - (target + 5));
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(h->orig, cur, (size_t)n);
    memcpy((void *)target, patch, (size_t)n);
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    h->target = target, h->n = n, h->on = 1;
    return 1;
}

static void hook_remove(hk_t *h)
{
    if (!h->on)
        return;
    DWORD old;
    if (VirtualProtect((void *)h->target, (SIZE_T)h->n, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void *)h->target, h->orig, (size_t)h->n);
        VirtualProtect((void *)h->target, (SIZE_T)h->n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void *)h->target, (SIZE_T)h->n);
    }
    h->on = 0;
}

/* ---- state ---- */
#define MAXCAP 4
typedef struct {
    int active;
    unsigned unit, ctl_obj, mgr, mgr2, think_obj;   /* mgr = unit+0xE80 (lower body machine owner), mgr2 = unit+0xF6C (upper body machine), think_obj = uCallThink at unit+0xFA4 */
    int saved_ctl, saved_kind, saved_tmode;
    int pad, role;
    int flip;   /* 1 = the character was taken over (fields changed, given back on release); 0 = passive: only wrapped with its pad slot (the other character is bound) */
    unsigned swaps;
    int lost;   /* the game took him back (setKind != 1 allowed with `cap keep 0`, or the fields changed and were not written back): his blows are an AI's */
    int aim_ok; /* aimgate1.h: may he aim now (FUN_006AD740 in his own phase), AG1_UNKNOWN before that */
} capu_t;
static capu_t caps[MAXCAP];

static struct {
    volatile int en, pad;
} roles[2];
static volatile int own_req, own_on, camera_follow = 1, blocked, force_raid, cam_restore;
static volatile unsigned own_slot;
static volatile unsigned view_idx[2];           /* per character (unit slot): DAT_0116C3E4 value while its swapped sections run, 0 = leave alone (split screen) */
static volatile int ext_req;                    /* another module (split.c) needs the own-slot hook installed */
/* threads whose own-slot queries answer a scoped slot: the second HUD's update phases (a cockpit and a minimap manager may update on two worker threads at once) */
#define SCOPE_MAX 8
static volatile LONG scope_tid[SCOPE_MAX], scope_slot[SCOPE_MAX], scope_n;
static volatile int remote_timeout_ms;
static unsigned captures_total;

static hk_t hk_pd, hk_v8, hk_v9, hk_v10, hk_think, hk_own;
/* uPlayer::setKind = vtable slot 0xF0 of the player classes, 0x6A7B40 (`56 57 8b 7c 24 0c`): 0x6CD4E0 writes the control type [[unit+0xE80]+0x38]+0x34 and the kind unit+0xE30,
 * then 0x49BE80(unit+0xF6C) the upper-body mode [unit+0xFA4]+0x34 - exactly the three fields cap_take sets to 1. No direct caller (virtual only); the only other writer of
 * unit+0xE30 is the constructor (0x6CC18C `mov [esi+0xe30], 2`). wp5 (2026-11-07): the host's puppet of the client read kind 2 ctl 2 five minutes after `capture: took`,
 * the same unit: so the game called setKind(2) on it. The hook logs every such call with its caller and, while `keep_capture` (default), turns it into setKind(1). */
static hk_t hk_kind;
static void (__attribute__((thiscall)) *orig_setkind)(void *self, int v);
static volatile int keep_capture = 1;
static volatile LONG kind_calls, kind_kept, kind_allowed, field_fixes;
static volatile unsigned kind_last_caller, kind_last_unit, kind_last_tick;
static volatile int kind_last_v = -1;
/* FUN_0088CB10 (sGamePad vf6, after the pads were polled): while sGamePad+0x874 ("keyboard mode") is non-zero it MEMSETS the raw pad data of BOTH pads (sGamePad+0x198 and +0x464, 0x2C bytes each:
 * held, edges, sticks) and refills the CURRENT slot from the keyboard. Keyboard mode is set by any keyboard / mouse activity and left only when the current pad has held bits, so with it on the
 * second pad never reaches PadData (live: Pad[1] raw 0 with the remote record accepted). Optional hook (kbskip_req, default off; see kbskip1.h): the function runs as stock, the raw block of the
 * kept pads (kbskip_mask, default pad slot 1 = the partner's) is saved before it and put back after it = only that pad's wipe is undone; the keyboard still fills the current slot (menus, pause).
 * The first version skipped the whole function: no keyboard input at all (duo7: title never left; duo6-9: Esc opened the pause menu, nothing closed it). */
static hk_t hk_kb;
/* aimgate1.h: the second human's own "aim allowed" byte inside his swapped sections (default on; `cap aimgate 0` = stock, lab A/B). can_aim = FUN_006AD740, set when its bytes match. */
static volatile int aimgate_on = 1;
static unsigned char (__attribute__((thiscall)) *can_aim)(void *unit, int arg);
static volatile LONG ag_sections, ag_allowed;
static volatile int ag_last[2] = {AG1_UNKNOWN, AG1_UNKNOWN};   /* per role: the last value written */
static volatile int kbskip_req;
static volatile unsigned kbskip_mask = KB1_DEFAULT_MASK;
static kb1_stats kb_st;   /* written by the game's pad update only */
static int raid_active(void);
static void (TC *orig_padData)(void *self, unsigned slot);
static void (FC *orig_v8)(unsigned), (FC *orig_v9)(unsigned), (FC *orig_v10)(unsigned), (FC *orig_think)(unsigned);
static unsigned (FC *orig_own)(unsigned self);
static int hooks_on;
static int inited;

/* ---- the swap lock ----
 * FOUND LIVE 2026-11-04: the unit phases run on EIGHT threads (the sUnit group loop queues the units of threaded groups as jobs; weapons and uCallThink units of the
 * players are among them, the player units themselves are on the logic thread). sGamePad+0x870 is one global: two threads inside swapped sections, or a worker reading
 * the pad while the logic thread has a section open, see the wrong slot (a character flickered into the aim state because of the other one's trigger) and a restore
 * of a stale "old" value left the slot at 1 for good. So: every swapped section runs under one lock (re-entrant for nested hooks on the same thread), and every player
 * unit is wrapped as soon as any role is bound (passively with its default pad slot when its own role is not bound). A thread that cannot get the lock within 30 ms
 * goes on unlocked (counted) rather than risk a deadlock. */
static CRITICAL_SECTION swap_cs;
static int swap_cs_ready;
static volatile LONG lock_waits, lock_timeouts;
static unsigned slot_leaks;
static DWORD swap_tids[8];
static volatile int n_swap_tids;

static int lock_take(void)
{
    if (!swap_cs_ready)
        return 0;
    if (TryEnterCriticalSection(&swap_cs))
        return 1;
    InterlockedIncrement(&lock_waits);
    DWORD t0 = GetTickCount();
    do {
        Sleep(0);
        if (TryEnterCriticalSection(&swap_cs))
            return 1;
    } while (GetTickCount() - t0 < 30);
    InterlockedIncrement(&lock_timeouts);
    return 0;
}

static void note_thread(void)
{
    DWORD t = GetCurrentThreadId();
    int n = n_swap_tids;
    for (int i = 0; i < n && i < 8; i++)
        if (swap_tids[i] == t)
            return;
    if (n < 8) {
        swap_tids[n] = t;
        n_swap_tids = n + 1;
    }
}

/* ---- capture of a unit ---- */
static capu_t *cap_find(unsigned unit)
{
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && caps[i].unit == unit)
            return &caps[i];
    return NULL;
}

static capu_t *cap_take(unsigned unit, int role)
{
    unsigned co = ctl_obj_of(unit);
    if (!co)
        return NULL;
    if (swap_cs_ready)
        EnterCriticalSection(&swap_cs);
    capu_t *found = cap_find(unit);
    if (found) {
        if (swap_cs_ready)
            LeaveCriticalSection(&swap_cs);
        return found;
    }
    for (int i = 0; i < MAXCAP; i++)
        if (!caps[i].active) {
            capu_t *c = &caps[i];
            memset(c, 0, sizeof *c);
            c->unit = unit, c->ctl_obj = co, c->role = role, c->flip = roles[role].en, c->pad = roles[role].en ? roles[role].pad : 0, c->mgr = rd32(unit + 0xE80), c->mgr2 = unit + 0xF6C;
            c->think_obj = rd32(unit + 0xFA4);
            c->aim_ok = AG1_UNKNOWN;
            if (!c->flip) {   /* passive: the character is not bound, nothing of it is changed; it only runs under its default pad slot, inside the lock */
                c->active = 1;
                if (swap_cs_ready)
                    LeaveCriticalSection(&swap_cs);
                return c;
            }
            c->saved_ctl = (int)rd32(co + 0x34), c->saved_kind = (int)rd32(unit + 0xE30);
            wr32(co + 0x34, 1);
            wr32(unit + 0xE30, 1);
            /* the upper-body state machine has its own control mode in its uCallThink unit: 2 = AI partner (AI variants of the states: fire = think+0x158 bit 0),
             * 1 = human (variants that read the pad) */
            if (c->think_obj) {
                c->saved_tmode = (int)rd32(c->think_obj + 0x34);
                wr32(c->think_obj + 0x34, 1);
            }
            c->active = 1;
            captures_total++;
            r1_log("capture: took unit %08x (slot field %u): control type %d -> 1, kind %d -> 1, upper mode %d -> 1, pad slot %d", unit, rd32(unit + 0xE2C), c->saved_ctl, c->saved_kind,
                   c->saved_tmode, c->pad);
            if (swap_cs_ready)
                LeaveCriticalSection(&swap_cs);
            return c;
        }
    if (swap_cs_ready)
        LeaveCriticalSection(&swap_cs);
    return NULL;
}

static void cap_release(capu_t *c)
{
    if (!c->active)
        return;
    if (c->flip && is_player_class(c->unit) && ctl_obj_of(c->unit) == c->ctl_obj) {
        wr32(c->ctl_obj + 0x34, (unsigned)c->saved_ctl);
        wr32(c->unit + 0xE30, (unsigned)c->saved_kind);
        if (c->think_obj && rd32(c->unit + 0xFA4) == c->think_obj)
            wr32(c->think_obj + 0x34, (unsigned)c->saved_tmode);
    }
    c->active = 0;
}

static void release_all(void)
{
    for (int i = 0; i < MAXCAP; i++)
        cap_release(&caps[i]);
}

/* hot path: called for every unit of every phase of every frame while a role is bound. Plain reads of the unit's own memory first, the class name only for candidates */
static capu_t *cap_for(unsigned unit)
{
    capu_t *c = cap_find(unit);
    if (c || !(roles[0].en | roles[1].en))
        return c;
    unsigned e = *(volatile unsigned *)(unit + 0xE2C), kind = *(volatile unsigned *)(unit + 0xE30);
    if (e > 1 || (kind != 1 && kind != 2) || !is_player_class(unit))
        return NULL;
    return cap_take(unit, (int)e);
}

/* the capture whose pad slot this unit runs under: the character itself, or its weapon (owner = unit+0x30) */
static capu_t *cap_scope(unsigned unit)
{
    capu_t *c = cap_for(unit);
    if (c || !(roles[0].en | roles[1].en))
        return c;
    unsigned owner = *(volatile unsigned *)(unit + 0x30);
    return owner ? cap_find(owner) : NULL;
}

static void swap_call(void (FC *fn)(unsigned), unsigned unit, capu_t *c)
{
    unsigned g = gp();
    if (!g) {
        fn(unit);
        return;
    }
    int locked = lock_take();
    note_thread();
    unsigned old = *(volatile unsigned *)(g + 0x870);
    *(volatile unsigned *)(g + 0x870) = (unsigned)c->pad;
    /* split screen: the view this character's per-view units (action prompts, message boxes) are made for; sCamera viewport lookups by "the current view" see the same index */
    unsigned vwant = AD.view_idx_ptr && (unsigned)c->role < 2 ? view_idx[c->role] : 0, vold = 0;
    if (vwant) {
        vold = *(volatile unsigned *)AD.view_idx_ptr;
        *(volatile unsigned *)AD.view_idx_ptr = vwant;
    }
    /* aimgate1.h: the "aim allowed" byte is the OWN human's; a captured human who is not the own one gets his own value for his sections (his character phases on the logic
     * thread ask FUN_006AD740 about him; his weapon / state-machine sections use that answer) */
    int ag_set = 0;
    unsigned char ag_old = 0;
    if (can_aim && ag1_applies(aimgate_on, c->flip && !c->lost, rd32(c->unit + 0xE2C), capture_own_effective())) {
        int mine = unit == c->unit, bag = mine && rd32(c->unit + AG1_BAG_OFF) != 0;
        int v = ag1_value(mine, bag, bag ? can_aim((void *)c->unit, 0) : 0, &c->aim_ok);
        if (v != AG1_UNKNOWN) {
            volatile unsigned char *f = (volatile unsigned char *)(g + AG1_FLAG_OFF);
            ag_old = *f;
            *f = (unsigned char)v;
            ag_set = 1;
            InterlockedIncrement(&ag_sections);
            if (v && ag_old != (unsigned char)v)
                InterlockedIncrement(&ag_allowed);
            if ((unsigned)c->role < 2)
                ag_last[c->role] = v;
        }
    }
    fn(unit);
    if (ag_set)
        *(volatile unsigned char *)(g + AG1_FLAG_OFF) = ag_old;
    if (vwant)
        *(volatile unsigned *)AD.view_idx_ptr = vold;
    *(volatile unsigned *)(g + 0x870) = old;
    c->swaps++;
    if (locked)
        LeaveCriticalSection(&swap_cs);
}

static void FC hook_v8(unsigned unit)
{
    capu_t *c = cap_scope(unit);
    if (c)
        swap_call(orig_v8, unit, c);
    else
        orig_v8(unit);
}
static void FC hook_v9(unsigned unit)
{
    capu_t *c = cap_scope(unit);
    if (c)
        swap_call(orig_v9, unit, c);
    else
        orig_v9(unit);
}
static void FC hook_v10(unsigned unit)
{
    capu_t *c = cap_scope(unit);
    if (c)
        swap_call(orig_v10, unit, c);
    else
        orig_v10(unit);
}

/* the state machine tick of a unit (uCallThink phase): the owner object is uCallThink+0x30 = unit+0xE80 (lower body) or unit+0xF6C (upper body) */
static void FC hook_think(unsigned unit)
{
    unsigned mgr = *(volatile unsigned *)(unit + 0x30);
    capu_t *c = NULL;
    for (int i = 0; i < MAXCAP && mgr; i++)
        if (caps[i].active && (caps[i].mgr == mgr || caps[i].mgr2 == mgr)) {
            c = &caps[i];
            break;
        }
    if (c)
        swap_call(orig_think, unit, c);
    else
        orig_think(unit);
}

/* PadData[k] is built from the raw pad of the current slot: build slot k from pad k */
static void TC hook_padData(void *self, unsigned slot)
{
    unsigned g = gp();
    if (g && slot < 2) {
        int locked = lock_take();
        unsigned old = *(volatile unsigned *)(g + 0x870);
        *(volatile unsigned *)(g + 0x870) = slot;
        orig_padData(self, slot);
        *(volatile unsigned *)(g + 0x870) = old;
        if (locked)
            LeaveCriticalSection(&swap_cs);
        return;
    }
    orig_padData(self, slot);
}

/* the keyboard-mode function with the kept pads (kbskip1.h). Under the swap lock like the builder, so the current slot (+0x870) the keyboard goes to is the one of the pass (0), not a slot a
 * swapped section of another thread has set for a moment */
static void FC __attribute__((force_align_arg_pointer)) hook_kb(unsigned self)
{
    unsigned mask = kbskip_req ? kbskip_mask : 0;
    int locked = mask ? lock_take() : 0;
    /* the trampoline itself: hook_install writes it before the jump goes live (a pointer set after hook_install returned could still be NULL in the first call) */
    kb1_call((kb1_fn)hk_kb.tramp, (uintptr_t)self, mask, &kb_st);
    if (locked)
        LeaveCriticalSection(&swap_cs);
}

/* own slot: the offline branch of FUN_00882D60 (~190 callers: doors, enemies, items, cutscenes, HUD, the engine's spawn of the human) answers our slot; no sCoopManager
 * field is written, so the Raid state of the game stays as it was */
static unsigned FC hook_own(unsigned self)
{
    if (scope_n && !blocked) {   /* scoped answer for the registered threads only: every other thread keeps the real own slot */
        LONG me = (LONG)GetCurrentThreadId();
        for (int i = 0; i < SCOPE_MAX; i++)
            if (scope_tid[i] == me)
                return (unsigned)scope_slot[i];
    }
    if (own_on && !blocked)
        return own_slot;
    return orig_own(self);
}

static void __attribute__((thiscall)) hook_setkind(void *self, int v)
{
    unsigned unit = (unsigned)(uintptr_t)self, caller = (unsigned)(uintptr_t)__builtin_return_address(0);
    capu_t *c = cap_find(unit);
    int pass_v = v, d = ck_setkind(c && c->flip, keep_capture, v, &pass_v);
    if (d != CK_PASS) {
        kind_last_caller = caller, kind_last_unit = unit, kind_last_v = v, kind_last_tick = GetTickCount();
        LONG n = InterlockedIncrement(d == CK_KEEP ? &kind_kept : &kind_allowed);
        if (d == CK_LOST) c->lost = 1;
        if (n <= 40)
            r1_log("capture: the game sets kind %d on captured unit %08x (slot field %u), caller %08x: %s (#%ld)", v, unit, rd32(unit + 0xE2C), caller,
                   d == CK_KEEP ? "KEPT 1, the capture stays" : "ALLOWED (cap keep 0), the capture is LOST", (long)n);
        orig_setkind(self, pass_v);
        return;
    }
    if (rd32(unit + 0xE2C) <= 1 && InterlockedIncrement(&kind_calls) <= 40)
        r1_log("capture: setKind(%d) on unit %08x (slot field %u, kind %u, %s), caller %08x", v, unit, rd32(unit + 0xE2C), rd32(unit + 0xE30), c ? "captured" : "not captured", caller);
    orig_setkind(self, v);
}

static int hooks_install(void)
{
    static const unsigned char b_pd[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    static const unsigned char b_v89[] = {0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x1C, 0x0E, 0x00, 0x00};
    static const unsigned char b_v10[] = {0x8B, 0x81, 0x1C, 0x0E, 0x00, 0x00, 0xC1, 0xE8, 0x1E};
    static const unsigned char b_think[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x30};
    static const unsigned char b_own[] = {0x8B, 0x81, 0xF4, 0x00, 0x00, 0x00};
    int ok = 1;
    ok &= hook_install(&hk_pd, AD.fn_padData, b_pd, 6, (void *)hook_padData);
    ok &= hook_install(&hk_v8, AD.fn_v8, b_v89, 9, (void *)hook_v8);
    ok &= hook_install(&hk_v9, AD.fn_v9, b_v89, 9, (void *)hook_v9);
    ok &= hook_install(&hk_v10, AD.fn_v10, b_v10, 9, (void *)hook_v10);
    ok &= hook_install(&hk_think, AD.fn_think, b_think, 6, (void *)hook_think);
    ok &= hook_install(&hk_own, AD.fn_own, b_own, 6, (void *)hook_own);
    orig_padData = (void (TC *)(void *, unsigned))hk_pd.tramp;
    orig_v8 = (void (FC *)(unsigned))hk_v8.tramp;
    orig_v9 = (void (FC *)(unsigned))hk_v9.tramp;
    orig_v10 = (void (FC *)(unsigned))hk_v10.tramp;
    orig_think = (void (FC *)(unsigned))hk_think.tramp;
    orig_own = (unsigned (FC *)(unsigned))hk_own.tramp;
    {   /* the aim gate's question (aimgate1.h), called, not hooked: only when its bytes are the supported build's */
        static const unsigned char b_aim[] = {0x83, 0xEC, 0x0C, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x86};
        unsigned char cur[sizeof b_aim];
        if (!can_aim && r1_safe_read((void *)AG1_CANAIM_FN, cur, sizeof cur) && !memcmp(cur, b_aim, sizeof b_aim))
            can_aim = (unsigned char (__attribute__((thiscall)) *)(void *, int))AG1_CANAIM_FN;
        if (!can_aim)
            r1_log("capture: 0x%X differs from the supported build: the second human's aim gate stays the own human's (stock)", AG1_CANAIM_FN);
    }
    {   /* optional: without it the capture works as before (only the game's setKind is not seen) */
        static const unsigned char b_kind[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C};
        if (hook_install(&hk_kind, 0x6A7B40u, b_kind, 6, (void *)hook_setkind))
            orig_setkind = (void (__attribute__((thiscall)) *)(void *, int))hk_kind.tramp;
        else
            r1_log("capture: setKind hook NOT installed (bytes differ): a reset of a captured human is still caught by the per-pass field check");
    }
    hooks_on = 1;
    return ok;
}

static void hooks_remove(void)
{
    hook_remove(&hk_kind);
    hook_remove(&hk_own);
    hook_remove(&hk_think);
    hook_remove(&hk_v10);
    hook_remove(&hk_v9);
    hook_remove(&hk_v8);
    hook_remove(&hk_pd);
    hooks_on = 0;
}

/* ---- Raid guard ---- */
static int game_mode(void)
{
    unsigned holder = rd32(AD.gamesys_ptr), data = holder ? rd32(holder + 0x24) : 0;
    return data ? (int)rd32(data + 0x20) : 0;
}
static int raid_active(void)
{
    if (force_raid)
        return 1;
    unsigned cm = rd32(AD.coopmgr_ptr), pm = cm ? rd32(cm + 0xF4) : 0xFFFFFFFFu;
    return game_mode() != 0 || pm == 1 || pm == 2;
}

/* ---- input sources ---- */
typedef struct {
    int kind;
    r1_input_t last;   /* what was published to the game last (remote: raw record + latched presses) */
    r1_input_t raw;    /* the last remote record as received */
    unsigned seq, ts, accepted, stale, timeouts;
    int have, neutralised, latched;
    DWORD recv_tick, hold_until[16], lt_until, rt_until;
    uint8_t lt_hold, rt_hold;
} src_t;
static src_t src[2];
static volatile int latch_ms = 40;

static void publish(int slot, const r1_input_t *r) { testmode_pad_set(slot, 1, r); }

/* A press that is shorter than the game's poll interval (one record pressed, the next one released, both between two polls) would never be seen: the game derives its
 * "pressed this frame" edges from poll to poll. A newly pressed button / trigger of a remote record therefore stays pressed for latch_ms (40) even when the next record releases it. */
static void publish_remote(int slot, DWORD now)
{
    src_t *s = &src[slot];
    r1_input_t e = s->raw;
    int latched = 0;
    for (int b = 0; b < 16; b++)
        if (!(e.buttons & (1u << b)) && (int)(s->hold_until[b] - now) > 0)
            e.buttons |= (uint16_t)(1u << b), latched = 1;
    if (e.lt < s->lt_hold && (int)(s->lt_until - now) > 0)
        e.lt = s->lt_hold, latched = 1;
    if (e.rt < s->rt_hold && (int)(s->rt_until - now) > 0)
        e.rt = s->rt_hold, latched = 1;
    s->latched = latched;
    s->last = e;
    publish(slot, &e);
}

void capture_set_source(int slot, int kind)
{
    if (slot < 0 || slot > 1)
        return;
    r1_input_t z;
    memset(&z, 0, sizeof z);
    if (kind == CAP_NATIVE) {
        src[slot].kind = CAP_NATIVE;
        testmode_pad_set(slot, 0, &z);   /* the synthetic pad goes away: the game's own XInput user <slot> is polled again */
        return;
    }
    src[slot].kind = kind;
    src[slot].last = src[slot].raw = z;
    src[slot].latched = 0;
    memset(src[slot].hold_until, 0, sizeof src[slot].hold_until);
    src[slot].lt_until = src[slot].rt_until = 0;
    src[slot].have = 0;
    src[slot].neutralised = 0;
    src[slot].recv_tick = GetTickCount();
    publish(slot, &z);   /* connected and neutral: the game assigns the pad slot to it */
}

int capture_push_remote(int slot, const r1_input_t *rec, unsigned seq, unsigned ts_ms)
{
    if (slot < 0 || slot > 1 || !rec)
        return 0;
    src_t *s = &src[slot];
    if (s->have && (int)(seq - s->seq) <= 0) {
        s->stale++;
        return 0;
    }
    DWORD now = GetTickCount();
    uint16_t newly = (uint16_t)(rec->buttons & ~s->raw.buttons);
    for (int b = 0; b < 16; b++)
        if (newly & (1u << b))
            s->hold_until[b] = now + (DWORD)latch_ms;
    if (rec->lt > s->raw.lt)
        s->lt_hold = rec->lt, s->lt_until = now + (DWORD)latch_ms;
    if (rec->rt > s->raw.rt)
        s->rt_hold = rec->rt, s->rt_until = now + (DWORD)latch_ms;
    s->raw = *rec;
    s->last = *rec;
    s->seq = seq, s->ts = ts_ms, s->have = 1, s->neutralised = 0;
    s->recv_tick = now;
    s->accepted++;
    if (s->kind == CAP_REMOTE)
        publish_remote(slot, now);
    return 1;
}

void capture_set_latch(int ms) { latch_ms = ms < 0 ? 0 : ms; }

void capture_set_remote_timeout(int ms) { remote_timeout_ms = ms; }

/* ---- scripted tracks (lab bot): timed records played as the source of a pad slot, the same funnel as a remote record ---- */
typedef struct {
    unsigned t_ms;
    r1_input_t r;
} trk_t;
typedef struct {
    trk_t *a;
    int n, cap, state, idx, loop, user; /* state 0 idle, 1 recording, 2 playing */
    DWORD t0, stop_at, last_ms;
} trk_slot_t;
static trk_slot_t tk[2];

static trk_t *trk_buf(int slot)
{
    if (!tk[slot].a) {
        tk[slot].cap = 20000;
        tk[slot].a = (trk_t *)calloc((size_t)tk[slot].cap, sizeof(trk_t));
    }
    return tk[slot].a;
}

static void trk_frame(void)
{
    for (int slot = 0; slot < 2; slot++) {
        trk_slot_t *t = &tk[slot];
        if (t->state == 0)
            continue;
        DWORD now = GetTickCount() - t->t0;
        if (t->state == 1) {
            r1_input_t r;
            memset(&r, 0, sizeof r);
            testmode_pad_peek(t->user, &r);
            if (t->n < t->cap && (t->n == 0 || memcmp(&r, &t->a[t->n - 1].r, sizeof r) != 0 || now - t->last_ms >= 100)) {
                t->a[t->n].t_ms = now, t->a[t->n].r = r, t->n++;
                t->last_ms = now;
            }
            if (now >= t->stop_at) {
                t->state = 0;
                r1_log("capture: track %d recorded: %d samples over %u ms", slot, t->n, (unsigned)now);
            }
        } else if (t->n > 0) {
            if (now > t->a[t->n - 1].t_ms + 200) {
                if (t->loop) {
                    t->t0 = GetTickCount(), t->idx = 0, now = 0;
                } else {
                    r1_input_t z;
                    memset(&z, 0, sizeof z);
                    src[slot].last = z;
                    publish(slot, &z);
                    t->state = 0;
                    r1_log("capture: track %d finished", slot);
                    continue;
                }
            }
            while (t->idx + 1 < t->n && t->a[t->idx + 1].t_ms <= now)
                t->idx++;
            src[slot].last = t->a[t->idx].r;
            publish(slot, &t->a[t->idx].r);
        }
    }
}

static int trk_play(int slot, int loop, char *out, int size)
{
    if (slot < 0 || slot > 1 || tk[slot].n < 1) {
        snprintf(out, size, "track %d: nothing recorded/loaded (or bad slot)", slot);
        return 0;
    }
    src[slot].kind = CAP_SCRIPT;
    tk[slot].loop = loop, tk[slot].idx = 0, tk[slot].t0 = GetTickCount(), tk[slot].state = 2;
    return 1;
}

/* ---- camera follow ---- */
static unsigned unit_by_slot(unsigned slot)
{
    unsigned sp = rd32(AD.splayer_ptr);
    if (!sp)
        return 0;
    unsigned head = rd32(sp + 0x30), endp = rd32(sp + 0x34);
    for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8)) {
        unsigned unit = rd32(head + 0x10);
        if (unit && rd32(unit + 0xE2C) == slot && is_player_class(unit))
            return unit;
    }
    return 0;
}

/* The stock camera does not follow the own slot (its target stays on the first character): a player-class target (not a cutscene / event camera target) is replaced
 * by the own slot's unit; written only when it differs */
static void cam_set_slot(unsigned slot)
{
    unsigned root = rd32(AD.camroot_ptr), mgr = root ? rd32(root + 0xCE0) : 0, want = unit_by_slot(slot);
    if (mgr && want && is_player_class(rd32(mgr + 0x74))) {
        wr32(mgr + 0x74, want);
        wr32(mgr + 0x78, want);
    }
}
static void cam_follow(void)
{
    if (!own_on || !camera_follow || blocked)
        return;
    unsigned root = rd32(AD.camroot_ptr), mgr = root ? rd32(root + 0xCE0) : 0;
    if (!mgr)
        return;
    unsigned t = rd32(mgr + 0x74);
    if (!t || !is_player_class(t))
        return;
    unsigned want = unit_by_slot(own_slot);
    if (want && want != t) {
        wr32(mgr + 0x74, want);
        wr32(mgr + 0x78, want);
    }
}

/* ---- the per-pass work (game thread) ---- */
static void apply(void)
{
    int raid = raid_active();
    if (raid && !blocked) {
        if (hooks_on) {
            release_all();
            hooks_remove();
        }
        blocked = CAP_BLOCK_RAID;
        r1_log("capture: Raid scene/session active (mode %d): hooks removed, nothing is touched", game_mode());
    } else if (!raid && blocked == CAP_BLOCK_RAID) {
        blocked = CAP_BLOCK_NONE;
        r1_log("capture: campaign again, capture may run");
    }
    int need = roles[0].en || roles[1].en || own_req || ext_req;
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && caps[i].flip && !roles[caps[i].role].en)
            cap_release(&caps[i]);
    if (blocked)
        return;
    if (need && !hooks_on) {
        if (!hooks_install()) {
            hooks_remove();
            blocked = CAP_BLOCK_BUILD;
            r1_log("capture: a hook point did not match the supported build, all hooks removed");
        } else {
            r1_log("capture: hooks installed (padData, dispatchers 8/9/10, state machine phase, own slot)");
        }
    } else if (!need && hooks_on) {
        release_all();
        hooks_remove();
        r1_log("capture: no role bound, hooks removed");
    }
}

void capture_set_kbskip(int on) { kbskip_req = on != 0; }
int capture_kbskip_state(void) { return kbskip_req ? (hk_kb.on ? 2 : 1) : (hk_kb.on ? 3 : 0); }
void capture_set_kbskip_pads(unsigned mask)
{
    mask &= 3u;
    kbskip_mask = mask ? mask : KB1_DEFAULT_MASK;
}
unsigned capture_kbskip_pads(void) { return kbskip_mask; }

void capture_kbskip_text(char *out, int size)
{
    snprintf(out, size, "keeps pad slot %s through the keyboard-mode wipe (the keyboard still fills the current slot); calls %u, in keyboard mode %u, kept %u, current slot was a kept pad %u "
             "(last current %u), keyboard mode now %u",
             kb1_mask_name(kbskip_mask), kb_st.calls, kb_st.kbcalls, kb_st.kept, kb_st.cur_hit, kb_st.last_cur, gp() ? rd32(gp() + KB1_MODE_OFF) : 0);
}

static void kbskip_apply(void)
{
    static const unsigned char b_kb[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    if (kbskip_req && !hk_kb.on && !raid_active()) {
        if (hook_install(&hk_kb, KB1_FN, b_kb, 6, (void *)hook_kb)) {
            r1_log("capture: kbskip hook on 0x88CB10: in keyboard mode the raw data of pad slot %s is kept, the keyboard still fills the current slot", kb1_mask_name(kbskip_mask));
        } else
            kbskip_req = 0;
    } else if ((!kbskip_req || raid_active()) && hk_kb.on) {
        hook_remove(&hk_kb);
        r1_log("capture: keyboard-mode wipe hook removed");
    }
}

void capture_frame(void)
{
    kbskip_apply();
    trk_frame();
    DWORD now = GetTickCount();
    int tmo = remote_timeout_ms;
    for (int s = 0; s < 2; s++)
        if (tmo > 0 && src[s].kind == CAP_REMOTE && !src[s].neutralised && (int)(now - src[s].recv_tick) > tmo) {
            r1_input_t z;
            memset(&z, 0, sizeof z);
            if (memcmp(&src[s].last, &z, sizeof z) != 0) {
                src[s].last = src[s].raw = z;
                src[s].latched = 0;
                memset(src[s].hold_until, 0, sizeof src[s].hold_until);
                src[s].lt_until = src[s].rt_until = 0;
                publish(s, &z);
                src[s].timeouts++;
            }
            src[s].neutralised = 1;
        }
    if (cam_restore) {
        cam_restore = 0;
        if (!blocked)
            cam_set_slot(0);
    }
    for (int s = 0; s < 2; s++)
        if (src[s].kind == CAP_REMOTE && src[s].latched)
            publish_remote(s, now);
    apply();
    if (!hooks_on)
        return;
    {   /* pass start: the workers are idle, nothing may hold a swapped slot; repair a leak (a restore that raced) */
        unsigned g = gp();
        if (g && *(volatile unsigned *)(g + 0x870) != 0) {
            if (++slot_leaks <= 5)
                r1_log("capture: sGamePad+0x870 was %u at the start of a pass (leak #%u), reset to 0", *(volatile unsigned *)(g + 0x870), slot_leaks);
            *(volatile unsigned *)(g + 0x870) = 0;
        }
        /* The game switches PadData[k] between "keyboard" (device kind 6, after a key was used for the menus) and "pad" in FUN_00888FD0, ONCE, when it sees pad activity while it is in
         * keyboard mode (sGamePad+0x874 = 1). If that moment passed with the wrong slot current (the leak above), the kind stays 6 and the pad of that slot is ignored forever
         * (the aim trigger read the keyboard map). A pad slot that shows input while its PadData says keyboard gets the keyboard mode back so the game's own transition runs again. */
        static DWORD last_fix;
        for (int s = 0; g && s < 2; s++)
            if (rd32(g + 0x638 + 0x90 * (unsigned)s) == 6 && rd32(g + 0x198 + 0x2CC * (unsigned)s) != 0 && rd32(g + 0x874) == 0 && GetTickCount() - last_fix > 500) {
                wr32(g + 0x874, 1);
                last_fix = GetTickCount();
                r1_log("capture: pad slot %d has input but PadData says keyboard: keyboard mode set so the game switches the device kind", s);
            }
    }
    cam_follow();
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && (!is_player_class(caps[i].unit) || ctl_obj_of(caps[i].unit) != caps[i].ctl_obj)) {
            r1_log("capture: unit %08x is gone (freed by a load / area change); the role stays bound", caps[i].unit);
            caps[i].active = 0;
        }
    /* a captured human whose control type / kind / upper mode is not 1 any more (a path that is not setKind, or setKind allowed): logged; written back while keep_capture */
    for (int i = 0; i < MAXCAP; i++) {
        capu_t *c = &caps[i];
        if (!c->active || !c->flip) continue;
        unsigned th = rd32(c->unit + 0xFA4), ct = rd32(c->ctl_obj + 0x34), kd = rd32(c->unit + 0xE30), tm = th ? rd32(th + 0x34) : 1;
        int d = ck_fields(ct, kd, tm, th, c->think_obj, keep_capture);
        if (d == CK_PASS) {
            if (c->lost && keep_capture) c->lost = 0;
            continue;
        }
        if (d == CK_LOST && c->lost) continue;   /* known (keep 0): logged once */
        LONG n = InterlockedIncrement(&field_fixes);
        if (d == CK_KEEP) {
            wr32(c->ctl_obj + 0x34, 1), wr32(c->unit + 0xE30, 1);
            if (th) wr32(th + 0x34, 1);
            c->think_obj = th, c->lost = 0;
        } else {
            c->lost = 1;
        }
        if (n <= 20)
            r1_log("capture: captured unit %08x (slot field %u) found with control type %u, kind %u, upper mode %u (think %08x, was %08x)%s", c->unit, rd32(c->unit + 0xE2C), ct, kd, tm, th,
                   c->think_obj, keep_capture ? ": written back to 1" : ": left (cap keep 0), the capture is LOST");
    }
}

/* the captured unit is driven by its pad slot right now (taken over, fields 1); 0 = not captured, passive, or the game took him back (lost) */
int capture_unit_human(unsigned unit)
{
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && caps[i].unit == unit)
            return caps[i].flip && !caps[i].lost && rd32(caps[i].ctl_obj + 0x34) == 1 && rd32(unit + 0xE30) == 1;
    return 0;
}

int capture_init(void)
{
    if (inited)
        return 0;
    inited = 1;
    InitializeCriticalSection(&swap_cs);
    swap_cs_ready = 1;
    lt_add_frame_cb(capture_frame);
    r1_log("capture: module ready (no hook is installed until a role or the own slot is requested)");
    return 0;
}

/* ---- API ---- */
void capture_bind(int unit_slot, int pad_slot)
{
    if (unit_slot < 0 || unit_slot > 1 || pad_slot < 0 || pad_slot > 1)
        return;
    roles[unit_slot].pad = pad_slot;
    roles[unit_slot].en = 1;
    for (int i = 0; i < MAXCAP; i++)   /* already seen: a passive entry is dropped (taken over properly at its next phase), a taken one moves to the new pad slot */
        if (caps[i].active && caps[i].role == unit_slot) {
            if (caps[i].flip)
                caps[i].pad = pad_slot;
            else
                caps[i].active = 0;
        }
}

void capture_unbind(int unit_slot)
{
    for (int e = 0; e < 2; e++)
        if (unit_slot < 0 || unit_slot == e)
            roles[e].en = 0;
    /* the captured units are given back by apply() on the game thread */
}

void capture_set_own(int unit_slot)
{
    if (unit_slot < 0) {
        int was = own_on;
        own_on = 0;
        own_req = 0;
        if (was && camera_follow)
            cam_restore = 1;   /* done on the game thread in the next pass */
        return;
    }
    own_slot = (unsigned)(unit_slot > 1 ? 1 : unit_slot);
    own_req = 1;
    own_on = 1;
}

void capture_set_camera_follow(int on) { camera_follow = on != 0; }

int capture_busy(void) { return hooks_on || roles[0].en || roles[1].en || own_req || ext_req; }

void capture_ext_request(int on) { ext_req = on != 0; }
void capture_set_view_index(int unit_slot, unsigned view)
{
    if (unit_slot >= 0 && unit_slot < 2)
        view_idx[unit_slot] = view;
}
int capture_hooks_ready(void) { return hooks_on && !blocked; }
void capture_own_scope(int slot)
{
    LONG me = (LONG)GetCurrentThreadId();
    if (slot < 0 || slot > 1 || !hooks_on) {   /* close this thread's scope */
        for (int i = 0; i < SCOPE_MAX; i++)
            if (scope_tid[i] == me) {
                scope_tid[i] = 0;
                InterlockedDecrement(&scope_n);
            }
        return;
    }
    for (int i = 0; i < SCOPE_MAX; i++)
        if (scope_tid[i] == me) {
            scope_slot[i] = slot;
            return;
        }
    for (int i = 0; i < SCOPE_MAX; i++)
        if (InterlockedCompareExchange(&scope_tid[i], me, 0) == 0) {
            scope_slot[i] = slot;
            InterlockedIncrement(&scope_n);
            return;
        }
}
int capture_own_effective(void)
{
    if (own_on && !blocked)
        return (int)own_slot;
    unsigned cm = rd32(AD.coopmgr_ptr), pm = cm ? rd32(cm + 0xF4) : 0;
    return pm == 1 || pm == 2 ? (int)(rd32(cm + 0xF8) & 1) : 0;   /* what 0x87E1B0 answers without our override: pm - 1 <= 1 ? [+0xF8] : 0 */
}

void capture_get_state(capture_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->hooks_on = hooks_on, st->blocked = blocked, st->own_on = own_on, st->own_slot = own_slot, st->camera_follow = camera_follow, st->captures = captures_total;
    st->game_mode = game_mode();
    DWORD now = GetTickCount();
    for (int e = 0; e < 2; e++) {
        st->role[e].enabled = roles[e].en, st->role[e].pad = roles[e].pad;
        for (int i = 0; i < MAXCAP; i++)
            if (caps[i].active && caps[i].role == e)
                st->role[e].unit = caps[i].unit, st->role[e].swaps = caps[i].swaps;
        st->src[e].kind = src[e].kind, st->src[e].last = src[e].last, st->src[e].seq = src[e].seq, st->src[e].accepted = src[e].accepted, st->src[e].stale = src[e].stale;
        st->src[e].timeouts = src[e].timeouts, st->src[e].ts = src[e].ts, st->src[e].age_ms = src[e].have ? now - src[e].recv_tick : 0, st->src[e].script_playing = tk[e].state == 2;
    }
}

void capture_status_text(char *out, int size)
{
    capture_state_t st;
    capture_get_state(&st);
    static const char *const kn[] = {"native", "remote", "script"};
    int o = snprintf(out, size, "capture: hooks %s%s, game mode %d, own slot %s%u (camera %s), remote timeout %d ms, %u units taken\n", st.hooks_on ? "ON" : "off",
                     st.blocked == CAP_BLOCK_RAID ? " BLOCKED (Raid)" : st.blocked == CAP_BLOCK_BUILD ? " BLOCKED (build mismatch)" : "", st.game_mode, st.own_on ? "" : "(default) ", st.own_slot,
                     st.camera_follow ? "follows" : "left alone", remote_timeout_ms, st.captures);
    o += snprintf(out + o, size - o, "swap lock: %ld waits, %ld timeouts, %u slot leaks repaired, %d threads ran swapped sections\n", (long)lock_waits, (long)lock_timeouts, slot_leaks, (int)n_swap_tids);
    for (int e = 0; e < 2; e++)
        o += snprintf(out + o, size - o, "role unit slot %d: %s pad slot %d, unit %08x, %u swaps\n", e, st.role[e].enabled ? "bound to" : "(unbound)", st.role[e].pad, st.role[e].unit, st.role[e].swaps);
    unsigned g = gp();
    for (int s = 0; s < 2; s++) {
        unsigned pd = g ? g + 0x3C + 0x2CC * (unsigned)s : 0;
        o += snprintf(out + o, size - o, "pad slot %d: source %s, last record %04x lt %u rt %u L(%d,%d) R(%d,%d), seq %u ts %u, accepted %u stale %u timeouts %u age %u ms%s; game Pad[%d] kind %u held %08x\n", s,
                      kn[st.src[s].kind], st.src[s].last.buttons, st.src[s].last.lt, st.src[s].last.rt, st.src[s].last.lx, st.src[s].last.ly, st.src[s].last.rx, st.src[s].last.ry, st.src[s].seq,
                      st.src[s].ts, st.src[s].accepted, st.src[s].stale, st.src[s].timeouts, st.src[s].age_ms, st.src[s].script_playing ? ", script playing" : "", s, pd ? rd32(pd + 4) >> 24 & 0xFF : 0,
                      pd ? rd32(pd + 0x15C) : 0);
    }
    if (o > 0 && o < size)
        o += snprintf(out + o, size - o, "aim gate %s%s: sections %ld, allowed against the own human's value %ld, last per role %d / %d; the game's byte now: enabled %u aim allowed %u\n",
                      aimgate_on ? "on" : "OFF (stock)", can_aim ? "" : " (FUN_006AD740 not matched: inactive)", (long)ag_sections, (long)ag_allowed, ag_last[0], ag_last[1],
                      g ? *(volatile unsigned char *)(g + AG1_EN_OFF) : 0, g ? *(volatile unsigned char *)(g + AG1_FLAG_OFF) : 0);
    if (o > 0 && o < size)
        o += snprintf(out + o, size - o, "kbskip %d (state %d: 0 off, 1 wanted, 2 hooked, 3 unhooking): ", kbskip_req, capture_kbskip_state());
    if (o > 0 && o < size)
        capture_kbskip_text(out + o, size - o);
}

/* ---- console: cap <sub> ... ---- */
static int slot_arg(const char *s)
{
    if (!strcmp(s, "main") || !strcmp(s, "jill"))
        return 0;
    if (!strcmp(s, "partner") || !strcmp(s, "parker"))
        return 1;
    return s[0] >= '0' && s[0] <= '9' ? atoi(s) : -1;
}

void capture_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "", a[200] = "", b[200] = "", c2[40] = "";
    sscanf(arg, "%15s %199s %199s %39s", sub, a, b, c2);
    if (!strcmp(sub, "bind")) {   /* bind <unit slot|main|partner> <pad slot> */
        int u = slot_arg(a), p = atoi(b);
        if (u < 0 || u > 1 || p < 0 || p > 1) {
            snprintf(out, size, "usage: cap bind <unit slot 0|1|main|partner> <pad slot 0|1>");
            return;
        }
        capture_bind(u, p);
        apply();
        snprintf(out, size, "character with slot field %d plays on pad slot %d (hooks %s)", u, p, hooks_on ? "on" : blocked ? "blocked" : "off");
    } else if (!strcmp(sub, "unbind")) {
        int u = a[0] && strcmp(a, "all") ? slot_arg(a) : -1;
        capture_unbind(u);
        apply();
        snprintf(out, size, "unbound %s (hooks %s)", u < 0 ? "all roles" : a, hooks_on ? "still on for the other role / own slot" : "off");
    } else if (!strcmp(sub, "own")) {   /* own <slot|off> [nocam] */
        if (!a[0] || !strcmp(a, "off")) {
            capture_set_own(-1);
            apply();
            snprintf(out, size, "own slot: game default");
        } else {
            capture_set_camera_follow(strcmp(b, "nocam") != 0);
            capture_set_own(atoi(a));
            apply();
            snprintf(out, size, "own slot %u (camera %s, hooks %s)", own_slot, camera_follow ? "follows" : "left alone", hooks_on ? "on" : blocked ? "blocked" : "off");
        }
    } else if (!strcmp(sub, "src")) {   /* src <pad slot> native|remote|script */
        int s = atoi(a);
        if (s < 0 || s > 1 || !(b[0])) {
            snprintf(out, size, "usage: cap src <pad slot 0|1> native|remote|script");
            return;
        }
        capture_set_source(s, !strcmp(b, "remote") ? CAP_REMOTE : !strcmp(b, "script") ? CAP_SCRIPT : CAP_NATIVE);
        snprintf(out, size, "pad slot %d source: %s", s, b);
    } else if (!strcmp(sub, "rec") || !strcmp(sub, "recseq")) {
        /* rec <slot> <hex buttons> <lt> <rt> <lx> <ly> <rx> <ry>   |   recseq <slot> <seq> <hex> <lt> ...: what the network layer does with capture_push_remote */
        int slot = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        unsigned seq = 0;
        char bt[16] = "0";
        int ok;
        if (!strcmp(sub, "rec")) {
            ok = sscanf(arg + 3, "%d %15s %d %d %d %d %d %d", &slot, bt, &lt, &rt, &lx, &ly, &rx, &ry) >= 1;
            seq = slot >= 0 && slot < 2 ? src[slot].seq + 1 : 0;
        } else {
            ok = sscanf(arg + 6, "%d %u %15s %d %d %d %d %d %d", &slot, &seq, bt, &lt, &rt, &lx, &ly, &rx, &ry) >= 2;
        }
        r1_input_t r = {(uint16_t)strtoul(bt, NULL, 16), (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry};
        if (!ok || slot < 0 || slot > 1) {
            snprintf(out, size, "usage: cap rec <slot> <hex buttons> <lt> <rt> <lx> <ly> <rx> <ry> | cap recseq <slot> <seq> <hex> ...");
            return;
        }
        int acc = capture_push_remote(slot, &r, seq, GetTickCount());
        snprintf(out, size, "record seq %u for pad slot %d: buttons %04x lt %d rt %d L(%d,%d) R(%d,%d): %s%s", seq, slot, r.buttons, r.lt, r.rt, r.lx, r.ly, r.rx, r.ry, acc ? "accepted" : "DROPPED (stale)",
                 acc && src[slot].kind != CAP_REMOTE ? " (stored only: the source is not remote)" : "");
    } else if (!strcmp(sub, "latch")) {
        capture_set_latch(atoi(a));
        snprintf(out, size, "a newly pressed remote button / trigger stays pressed for at least %d ms", latch_ms);
    } else if (!strcmp(sub, "timeout")) {
        capture_set_remote_timeout(atoi(a));
        snprintf(out, size, "remote dead-man timeout %d ms (0 = off)", remote_timeout_ms);
    } else if (!strcmp(sub, "kbskip")) {
        /* kbskip 1|0 [1|0|both]: keep the raw data of a pad (default pad slot 1, the partner's) through the engine's keyboard-mode wipe (0x88CB10) without the puppet (lab: console keys turn
         * keyboard mode on); the keyboard still fills the current slot. `both` also keeps pad 0 = no keyboard input on pad 0 (the old whole skip; lab only). No argument: status */
        if (b[0]) {
            unsigned m = !strcmp(b, "both") ? 3u : !strcmp(b, "0") ? 1u : !strcmp(b, "1") ? 2u : 0u;
            if (!m) {
                snprintf(out, size, "usage: cap kbskip 1|0 [kept pad slot 1 (default) | 0 | both]");
                return;
            }
            capture_set_kbskip_pads(m);
        }
        if (a[0])
            capture_set_kbskip(atoi(a));
        int o = snprintf(out, size, "kbskip %d requested (state %d: 0 off, 1 wanted, 2 hooked, 3 unhooking); ", kbskip_req, capture_kbskip_state());
        if (o > 0 && o < size)
            capture_kbskip_text(out + o, size - o);
    } else if (!strcmp(sub, "aimgate")) {   /* aimgate 1|0: the second human's own "aim allowed" byte in his sections (aimgate1.h, default 1); 0 = stock (lab A/B) */
        if (a[0])
            aimgate_on = atoi(a) != 0;
        snprintf(out, size, "aim gate %s (%s); sections %ld, allowed against the own human's value %ld", aimgate_on ? "on" : "OFF (stock: the own human's value for everybody)",
                 can_aim ? "FUN_006AD740 matched" : "FUN_006AD740 not matched or hooks not installed yet", (long)ag_sections, (long)ag_allowed);
    } else if (!strcmp(sub, "keep")) {   /* keep 0|1: a setKind(!= 1) on a captured human is turned into 1 (default) / allowed (lab A/B) */
        if (a[0]) keep_capture = atoi(a) != 0;
        snprintf(out, size, "keep capture %d (setKind hook %s): setKind calls on slots 0/1 seen %ld, kept %ld, allowed %ld, field fixes %ld; last reset: unit %08x kind %d caller %08x at %lu (now %lu)",
                 keep_capture, hk_kind.on ? "in" : "not in", (long)kind_calls, (long)kind_kept, (long)kind_allowed, (long)field_fixes, kind_last_unit, kind_last_v, kind_last_caller,
                 (unsigned long)kind_last_tick, (unsigned long)GetTickCount());
    } else if (!strcmp(sub, "raid")) {   /* raid 1|0: pretend a Raid scene (lab proof of the guard: it only REMOVES our hooks) */
        force_raid = atoi(a);
        apply();
        snprintf(out, size, "Raid guard test %s: hooks %s, blocked %d", force_raid ? "ON" : "off", hooks_on ? "on" : "off", blocked);
    } else if (!strcmp(sub, "trk")) {
        char tsub[16] = "", ta[200] = "", tb[200] = "";
        int n1 = 0, n2 = 0;
        sscanf(arg + 3, "%15s %199s %199s", tsub, ta, tb);
        n1 = atoi(ta), n2 = atoi(tb);
        if (!strcmp(tsub, "rec")) {   /* trk rec <slot> <xinput user> <sec> */
            int sec = 0;
            sscanf(arg + 3, "%*s %*s %*s %d", &sec);
            if (n1 < 0 || n1 > 1) {
                snprintf(out, size, "usage: cap trk rec <track slot 0|1> <xinput user> <sec>");
                return;
            }
            trk_buf(n1);
            tk[n1].user = n2, tk[n1].n = 0, tk[n1].idx = 0, tk[n1].last_ms = 0, tk[n1].t0 = GetTickCount(), tk[n1].stop_at = (DWORD)(sec > 0 ? sec : 10) * 1000, tk[n1].state = 1;
            snprintf(out, size, "recording the pad of XInput user %d into track %d for %d s", n2, n1, sec > 0 ? sec : 10);
        } else if (!strcmp(tsub, "play")) {   /* trk play <slot> [loop] */
            if (trk_play(n1, n2 != 0, out, size))
                snprintf(out, size, "playing track %d: %d samples (%u ms) as the source of pad slot %d%s", n1, tk[n1].n, tk[n1].a[tk[n1].n - 1].t_ms, n1, n2 ? ", looped" : "");
        } else if (!strcmp(tsub, "playall")) {   /* both tracks start in the same pass */
            int loop = atoi(ta), ok = 1;
            for (int s = 0; s < 2; s++)
                if (tk[s].n > 0)
                    ok &= trk_play(s, loop != 0, out, size);
            DWORD t0 = GetTickCount();
            for (int s = 0; s < 2; s++)
                if (tk[s].state == 2)
                    tk[s].t0 = t0;
            snprintf(out, size, "playing tracks: slot 0 %d samples, slot 1 %d samples%s", tk[0].n, tk[1].n, loop ? ", looped" : "");
            (void)ok;
        } else if (!strcmp(tsub, "stop")) {
            for (int s = 0; s < 2; s++)
                if (ta[0] == 0 || n1 == s) {
                    if (tk[s].state == 2) {
                        r1_input_t z;
                        memset(&z, 0, sizeof z);
                        src[s].last = z;
                        publish(s, &z);
                    }
                    tk[s].state = 0;
                }
            snprintf(out, size, "tracks stopped (held: %d / %d samples)", tk[0].n, tk[1].n);
        } else if (!strcmp(tsub, "save") || !strcmp(tsub, "load")) {   /* trk save|load <slot> <file> */
            FILE *f = n1 >= 0 && n1 < 2 ? fopen(tb, !strcmp(tsub, "save") ? "w" : "r") : NULL;
            if (!f) {
                snprintf(out, size, "cannot open '%s' (usage: cap trk %s <slot> <file>, no spaces in the file name)", tb, tsub);
                return;
            }
            trk_buf(n1);
            if (!strcmp(tsub, "save")) {
                for (int i = 0; i < tk[n1].n; i++)
                    fprintf(f, "%u %04x %u %u %d %d %d %d\n", tk[n1].a[i].t_ms, tk[n1].a[i].r.buttons, tk[n1].a[i].r.lt, tk[n1].a[i].r.rt, tk[n1].a[i].r.lx, tk[n1].a[i].r.ly, tk[n1].a[i].r.rx,
                            tk[n1].a[i].r.ry);
            } else {
                tk[n1].n = 0;
                unsigned t, bt2, lt, rt;
                int lx, ly, rx, ry;
                while (tk[n1].n < tk[n1].cap && fscanf(f, "%u %x %u %u %d %d %d %d", &t, &bt2, &lt, &rt, &lx, &ly, &rx, &ry) == 8) {
                    tk[n1].a[tk[n1].n].t_ms = t;
                    tk[n1].a[tk[n1].n].r = (r1_input_t){(uint16_t)bt2, (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry};
                    tk[n1].n++;
                }
            }
            fclose(f);
            snprintf(out, size, "%s %d samples %s %s (track %d)", !strcmp(tsub, "save") ? "saved" : "loaded", tk[n1].n, !strcmp(tsub, "save") ? "to" : "from", tb, n1);
        } else {
            snprintf(out, size, "tracks: slot 0 %d samples (%s), slot 1 %d samples (%s); usage: cap trk rec <slot> <user> <sec> | play <slot> [loop] | playall [loop] | stop [slot] | save|load <slot> <file>", tk[0].n,
                     tk[0].state == 1 ? "recording" : tk[0].state == 2 ? "playing" : "idle", tk[1].n, tk[1].state == 1 ? "recording" : tk[1].state == 2 ? "playing" : "idle");
        }
    } else {
        capture_status_text(out, size);
    }
}
