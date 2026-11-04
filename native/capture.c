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

#define TC __attribute__((thiscall))
#define FC __attribute__((fastcall))

static capture_addr_t AD = {0x11E0388u, 0x1169394u, 0x1169108u, 0x1169398u, 0x11E00B8u, 0x88A470u, 0x6CCAB0u, 0x6C9C30u, 0x6C9C10u, 0x6C9800u, 0x87E1B0u};

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
    unsigned swaps;
} capu_t;
static capu_t caps[MAXCAP];

static struct {
    volatile int en, pad;
} roles[2];
static volatile int own_req, own_on, camera_follow = 1, blocked, force_raid, cam_restore;
static volatile unsigned own_slot;
static volatile int remote_timeout_ms;
static unsigned captures_total;

static hk_t hk_pd, hk_v8, hk_v9, hk_v10, hk_think, hk_own;
static void (TC *orig_padData)(void *self, unsigned slot);
static void (FC *orig_v8)(unsigned), (FC *orig_v9)(unsigned), (FC *orig_v10)(unsigned), (FC *orig_think)(unsigned);
static unsigned (FC *orig_own)(unsigned self);
static int hooks_on;
static int inited;

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
    for (int i = 0; i < MAXCAP; i++)
        if (!caps[i].active) {
            capu_t *c = &caps[i];
            memset(c, 0, sizeof *c);
            c->unit = unit, c->ctl_obj = co, c->role = role, c->pad = roles[role].pad, c->mgr = rd32(unit + 0xE80), c->mgr2 = unit + 0xF6C;
            c->saved_ctl = (int)rd32(co + 0x34), c->saved_kind = (int)rd32(unit + 0xE30);
            wr32(co + 0x34, 1);
            wr32(unit + 0xE30, 1);
            /* the upper-body state machine has its own control mode in its uCallThink unit: 2 = AI partner (AI variants of the states: fire = think+0x158 bit 0),
             * 1 = human (variants that read the pad) */
            c->think_obj = rd32(unit + 0xFA4);
            if (c->think_obj) {
                c->saved_tmode = (int)rd32(c->think_obj + 0x34);
                wr32(c->think_obj + 0x34, 1);
            }
            c->active = 1;
            captures_total++;
            r1_log("capture: took unit %08x (slot field %u): control type %d -> 1, kind %d -> 1, upper mode %d -> 1, pad slot %d", unit, rd32(unit + 0xE2C), c->saved_ctl, c->saved_kind,
                   c->saved_tmode, c->pad);
            return c;
        }
    return NULL;
}

static void cap_release(capu_t *c)
{
    if (!c->active)
        return;
    if (is_player_class(c->unit) && ctl_obj_of(c->unit) == c->ctl_obj) {
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
    if (e > 1 || !roles[e].en || (kind != 1 && kind != 2) || !is_player_class(unit))
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
    unsigned old = *(volatile unsigned *)(g + 0x870);
    *(volatile unsigned *)(g + 0x870) = (unsigned)c->pad;
    fn(unit);
    *(volatile unsigned *)(g + 0x870) = old;
    c->swaps++;
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
        unsigned old = *(volatile unsigned *)(g + 0x870);
        *(volatile unsigned *)(g + 0x870) = slot;
        orig_padData(self, slot);
        *(volatile unsigned *)(g + 0x870) = old;
        return;
    }
    orig_padData(self, slot);
}

/* own slot: the offline branch of FUN_00882D60 (~190 callers: doors, enemies, items, cutscenes, HUD, the engine's spawn of the human) answers our slot; no sCoopManager
 * field is written, so the Raid state of the game stays as it was */
static unsigned FC hook_own(unsigned self)
{
    if (own_on && !blocked)
        return own_slot;
    return orig_own(self);
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
    hooks_on = 1;
    return ok;
}

static void hooks_remove(void)
{
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
    int need = roles[0].en || roles[1].en || own_req;
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && !roles[caps[i].role].en)
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

void capture_frame(void)
{
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
    cam_follow();
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && (!is_player_class(caps[i].unit) || ctl_obj_of(caps[i].unit) != caps[i].ctl_obj)) {
            r1_log("capture: unit %08x is gone (freed by a load / area change); the role stays bound", caps[i].unit);
            caps[i].active = 0;
        }
}

int capture_init(void)
{
    if (inited)
        return 0;
    inited = 1;
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
    for (int i = 0; i < MAXCAP; i++)   /* already captured: move to the new pad slot */
        if (caps[i].active && caps[i].role == unit_slot)
            caps[i].pad = pad_slot;
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

int capture_busy(void) { return hooks_on || roles[0].en || roles[1].en || own_req; }

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
