/*
 * Hot-loadable plug-in: partner capture (stage 3) and live debugging tools. Loaded with the console "plug load <path>/r1plug_N.dll", commands "P <name> ...".
 * It installs its own inline hooks and removes them when unloaded, so the capture logic can change without restarting the game.
 *
 * Findings it builds on (notes\stage1-live.md, notes\stage3.md):
 *   - uPlayer::updatePad (0x6AFBD0) builds the unit's pad group (+0x1138..+0x1151: move xy, rotate xy, aim xy, run byte, aim byte) from the control type
 *     [[unit+0xE80]+0x38]+0x34: 1 = input manager, 2 = AI virtual pad (think+0x50..), 3 = remote (net fields).
 *   - every reader of the input manager (buttons: inline reads of Pad[sGamePad+0x870].held; sticks: PadData[+0x870]) indexes by the "current slot"
 *     sGamePad+0x870 (always 0 in play). The per-slot PadData (sGamePad+0x628+0x90*k) is built by FUN_0088a470(this = &PadData[k], k) every frame, but from
 *     the raw pad of the CURRENT slot: "padfix on" makes it read pad k for slot k.
 *   - capture of a unit: control type 1 and kind 1; its whole frame update (0x6CDD70, which includes updatePad and the button readers of the state
 *     machine) runs with +0x870 = slot (mode 2), or only updatePad does (mode 0), or the group is written from the input record after updatePad (mode 1).
 *   - the capture is a ROLE: while it is armed, any unit that is an AI partner (kind 2, class uPl<..>) gets captured the first time one of the hooks sees
 *     it, so loads, checkpoints and area changes (units are recreated) need no separate re-arm.
 *   - the input of the captured unit comes from an input source (coop_input.h): the local pad of XInput user <slot>, or a record handed in by the
 *     network layer (`P rec` simulates it); a record is published as the synthetic XInput user <slot> and goes through the game's own pad code.
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "r1.h"
#include "coop_input.h"

#define TC __attribute__((thiscall))
#define FC __attribute__((fastcall))

static const r1_api_t *A;

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    A->safe_read((void *)a, &v, 4);
    return v;
}
static unsigned rd8(unsigned a)
{
    unsigned char v = 0;
    A->safe_read((void *)a, &v, 1);
    return v;
}
static float rdf(unsigned a)
{
    float v = 0;
    A->safe_read((void *)a, &v, 4);
    return v;
}
static int wr(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)a, p, n, &got) && got == n;
}
static int wr32(unsigned a, unsigned v) { return wr(a, &v, 4); }
static int wr8(unsigned a, unsigned v)
{
    unsigned char c = (unsigned char)v;
    return wr(a, &c, 1);
}
static int wrf(unsigned a, float v) { return wr(a, &v, 4); }
static unsigned hx(const char *s) { return (unsigned)strtoul(s, NULL, 16); }

/* ---- inline hooks (restorable) ---- */
typedef struct {
    unsigned target;
    int n;
    unsigned char orig[16];
    void *tramp;
    int on;
} hk_t;

static int hook_install(hk_t *h, unsigned target, const unsigned char *expect, int n, void *hook)
{
    unsigned char cur[16];
    if (!A->safe_read((void *)target, cur, (unsigned)n) || memcmp(cur, expect, (size_t)n) != 0) {
        A->log("plug_cap: bytes at %08x differ from the expected build, not patching", target);
        return 0;
    }
    unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t)
        return 0;
    memcpy(t, cur, (size_t)n);
    t[n] = 0xE9;
    *(int *)(t + n + 1) = (int)((target + (unsigned)n) - (unsigned)(t + n + 5));
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    unsigned char *p = (unsigned char *)target;
    memcpy(h->orig, cur, (size_t)n);
    for (int i = 5; i < n; i++)
        p[i] = 0x90;
    *(int *)(p + 1) = (int)((unsigned)hook - (target + 5));
    p[0] = 0xE9;
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    h->target = target, h->n = n, h->tramp = t, h->on = 1;
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

/* ---- units ---- */
#define GAMEPAD_PTR 0x11E0388u

static unsigned gp(void) { return rd32(GAMEPAD_PTR); }
static unsigned ctl_obj_of(unsigned unit) { return rd32(rd32(unit + 0xE80) + 0x38); }

static int is_player_class(unsigned unit)
{
    char cn[96];
    return A->object_name(unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4);
}

static unsigned find_unit(const char *who)
{
    unsigned sp = rd32(0x1169394);
    if (!sp)
        return 0;
    if (who[0] >= '0' && who[0] <= '9')
        return hx(who);
    unsigned head = rd32(sp + 0x30), endp = rd32(sp + 0x34);
    for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8)) {
        unsigned unit = rd32(head + 0x10);
        char cn[96];
        if (!A->object_name(unit, cn, sizeof cn) || strncmp(cn, "uPl<", 4))
            continue;
        if (strstr(cn, who) || (!strcmp(who, "partner") && rd32(unit + 0xE30) == 2) || (!strcmp(who, "main") && rd32(unit + 0xE30) == 1))
            return unit;
    }
    return 0;
}

#include "plug_dbg.inc"

/* ---- input sources ---- */
static r1_input_src_t src[2];

static void input_get(int slot, r1_input_t *r)
{
    memset(r, 0, sizeof *r);
    if (slot < 0 || slot > 1)
        return;
    if (src[slot].kind == R1_SRC_REMOTE)
        *r = src[slot].last;
    else
        A->pad_peek(src[slot].arg, r);
}

/* a record arrives (network thread or `P rec`): remember it and publish it as the state of the synthetic XInput user <slot> */
static void input_remote_set(int slot, const r1_input_t *rec)
{
    if (slot < 0 || slot > 1)
        return;
    src[slot].last = *rec;
    src[slot].seq++;
    if (src[slot].kind == R1_SRC_REMOTE && A->version >= 2 && A->pad_set)
        A->pad_set(slot, 1, rec);
}

__declspec(dllexport) void r1in_remote_set(int slot, const r1_input_t *rec) { input_remote_set(slot, rec); }

/* ---- the capture ---- */
static hk_t hk_pad, hk_upd, hk_pd;
static void (FC *orig_updatePad)(unsigned unit);
static void (FC *orig_unitUpdate)(unsigned unit);
static void (TC *orig_padData)(void *self, unsigned slot);

#define MAXCAP 4
typedef struct {
    int active;
    unsigned unit, ctl_obj;
    int saved_ctl, saved_kind;
    unsigned swaps, writes;
} capu_t;
static capu_t caps[MAXCAP];
static struct {
    volatile int armed;   /* the role: capture every AI partner (kind 2, class uPl<>) the hooks see */
    int slot, mode;
    float dz;
    unsigned captures;
} role = {.dz = 0.24f, .slot = 1, .mode = 2};
static volatile int padfix;
static unsigned padfix_calls;

static capu_t *cap_find(unsigned unit)
{
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && caps[i].unit == unit)
            return &caps[i];
    return NULL;
}

static capu_t *cap_take(unsigned unit)
{
    unsigned co = ctl_obj_of(unit);
    if (!co)
        return NULL;
    for (int i = 0; i < MAXCAP; i++)
        if (!caps[i].active) {
            capu_t *c = &caps[i];
            memset(c, 0, sizeof *c);
            c->unit = unit, c->ctl_obj = co;
            c->saved_ctl = (int)rd32(co + 0x34), c->saved_kind = (int)rd32(unit + 0xE30);
            wr32(co + 0x34, 1);
            wr32(unit + 0xE30, 1);
            c->active = 1;
            role.captures++;
            A->log("plug_cap: captured unit %08x (slot field %u): control type %d -> 1, kind %d -> 1, pad slot %d mode %d", unit, rd32(unit + 0xE2C), c->saved_ctl, c->saved_kind, role.slot, role.mode);
            return c;
        }
    return NULL;
}

/* called by the hooks for every update of a player unit: finds the unit's capture, or captures it when the role is armed and it is an AI partner */
static capu_t *cap_for(unsigned unit)
{
    capu_t *c = cap_find(unit);
    if (c)
        return c;
    if (!role.armed || rd32(unit + 0xE30) != 2 || !is_player_class(unit))
        return NULL;
    return cap_take(unit);
}

static void cap_release(capu_t *c)
{
    if (!c->active)
        return;
    if (is_player_class(c->unit) && ctl_obj_of(c->unit) == c->ctl_obj) {
        wr32(c->ctl_obj + 0x34, (unsigned)c->saved_ctl);
        wr32(c->unit + 0xE30, (unsigned)c->saved_kind);
    }
    c->active = 0;
}

static void TC hook_padData(void *self, unsigned slot)
{
    unsigned g = gp();
    if (padfix && g && slot < 2) {
        unsigned old = rd32(g + 0x870);
        wr32(g + 0x870, slot);
        orig_padData(self, slot);
        wr32(g + 0x870, old);
        padfix_calls++;
        return;
    }
    orig_padData(self, slot);
}

static void swap_call(void (FC *fn)(unsigned), unsigned unit, capu_t *c)
{
    unsigned g = gp();
    if (!g) {
        fn(unit);
        return;
    }
    unsigned old = rd32(g + 0x870);
    wr32(g + 0x870, (unsigned)role.slot);
    fn(unit);
    wr32(g + 0x870, old);
    c->swaps++;
}

static float dead(float v, float dz)
{
    float a = fabsf(v);
    if (a < dz)
        return 0;
    float r = (a - dz) / (1.0f - dz);
    return v < 0 ? -r : r;
}

/* mode 1: the unit's pad group from an input record, with our own scaling (the PR #115 recipe) */
static void write_group(unsigned unit, capu_t *c)
{
    r1_input_t rec;
    input_get(role.slot, &rec);
    float lx = rec.lx / 32767.0f, ly = rec.ly / 32767.0f, rx = rec.rx / 32767.0f, ry = rec.ry / 32767.0f;
    float mag = sqrtf(lx * lx + ly * ly);
    float mv_x = 0, mv_y = 0;
    if (mag > role.dz) {
        float k = (mag - role.dz) / (1.0f - role.dz) / mag;
        mv_x = lx * k, mv_y = ly * k;
    }
    wrf(unit + 0x1138, mv_x), wrf(unit + 0x113C, mv_y);
    wrf(unit + 0x1140, dead(rx, role.dz)), wrf(unit + 0x1144, dead(ry, role.dz));
    wrf(unit + 0x1148, dead(rx, role.dz)), wrf(unit + 0x114C, dead(ry, role.dz));
    /* run: the game's own "may run" test, and the stick pushed far */
    unsigned char may_run = ((unsigned char (FC *)(unsigned))0x6AD9E0)(unit);
    wr8(unit + 0x1150, may_run && mag > 0.7f);
    /* aim: weapon allows aiming (FUN_006ad740, __thiscall with one stack argument (0), `ret 4`) and the left trigger is held */
    unsigned char may_aim = ((unsigned char (TC *)(unsigned, unsigned))0x6AD740)(unit, 0);
    wr8(unit + 0x1151, may_aim && rec.lt > 100);
    c->writes++;
}

static void FC hook_updatePad(unsigned unit)
{
    capu_t *c = cap_for(unit);
    if (c) {
        if (role.mode == 1) {
            orig_updatePad(unit);
            write_group(unit, c);
            return;
        }
        if (role.mode == 0) {
            swap_call(orig_updatePad, unit, c);
            return;
        }
    }
    orig_updatePad(unit);
}

static void FC hook_unitUpdate(unsigned unit)
{
    capu_t *c = cap_for(unit);
    if (c && role.mode == 2) {
        swap_call(orig_unitUpdate, unit, c);
        return;
    }
    orig_unitUpdate(unit);
}

static void cap_off_all(char *out, int size)
{
    int n = 0, o = 0;
    role.armed = 0;
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active) {
            if (o < size - 120)
                o += snprintf(out + o, size - o, "released %08x: control type back to %d, kind %d (%u swaps, %u writes); ", caps[i].unit, caps[i].saved_ctl, caps[i].saved_kind, caps[i].swaps, caps[i].writes);
            cap_release(&caps[i]);
            n++;
        }
    if (!n)
        snprintf(out, size, "capture was off (role disarmed)");
}

/* once per logic pass: drop captures of units that no longer exist (freed by a load / area change); the role stays armed and takes the new ones */
static void on_frame_(void)
{
    for (int i = 0; i < MAXCAP; i++)
        if (caps[i].active && (!is_player_class(caps[i].unit) || ctl_obj_of(caps[i].unit) != caps[i].ctl_obj)) {
            A->log("plug_cap: captured unit %08x is gone (class or control object changed); role %s", caps[i].unit, role.armed ? "stays armed" : "off");
            caps[i].active = 0;
        }
}

static int on_cmd(const char *name, const char *arg, char *out, int size)
{
    if (dbg_cmd(name, arg, out, size))
        return 1;
    if (!strcmp(name, "cap")) {
        char sub[16] = "", who[40] = "";
        int slot = 1, mode = 2;
        sscanf(arg, "%15s %39s %d %d", sub, who, &slot, &mode);
        if (!strcmp(sub, "on")) {
            /* "cap on partner|Parker|Jill|<hexaddr> [padslot=1] [mode]": arms the ROLE (every AI partner the hooks see is taken) and takes the named unit now */
            if (role.armed) {
                snprintf(out, size, "capture already on (cap off first)");
                return 1;
            }
            unsigned unit = find_unit(who);
            if (!unit || !is_player_class(unit) || !ctl_obj_of(unit)) {
                snprintf(out, size, "no such unit '%s' (cap on partner|main|Parker|Jill|<hexaddr> [padslot=1] [mode 2 swap whole update|0 swap updatePad|1 write group])", who);
                return 1;
            }
            role.slot = slot, role.mode = mode, role.armed = 1;
            capu_t *c = cap_find(unit);
            if (!c)
                c = cap_take(unit);
            snprintf(out, size, "capture ON: unit %08x pad slot %d mode %d (padfix %s), role armed: %s", unit, slot, mode, padfix ? "on" : "off", c ? "yes" : "no free slot");
        } else if (!strcmp(sub, "off")) {
            cap_off_all(out, size);
        } else if (!strcmp(sub, "dz")) {
            role.dz = (float)atof(who);
            snprintf(out, size, "dead zone %.2f", role.dz);
        } else {
            int o = snprintf(out, size, "role %s slot %d mode %d, %u captures, padfix %s (%u calls), hooks: updatePad %d unitUpdate %d padData %d\n", role.armed ? "ARMED" : "off", role.slot, role.mode, role.captures,
                             padfix ? "on" : "off", padfix_calls, hk_pad.on, hk_upd.on, hk_pd.on);
            for (int i = 0; i < MAXCAP; i++)
                if (caps[i].active)
                    o += snprintf(out + o, size - o, "unit %08x ctlobj %08x saved ctl %d kind %d now ctl %u kind %u, %u swaps, %u group writes\n", caps[i].unit, caps[i].ctl_obj, caps[i].saved_ctl, caps[i].saved_kind,
                                  rd32(caps[i].ctl_obj + 0x34), rd32(caps[i].unit + 0xE30), caps[i].swaps, caps[i].writes);
        }
        return 1;
    }
    if (!strcmp(name, "padfix")) {
        padfix = !strcmp(arg, "on");
        snprintf(out, size, "padfix %s", padfix ? "on: PadData[k] is built from pad k" : "off");
        return 1;
    }
    if (!strcmp(name, "src")) {   /* src <slot> pad <user> | src <slot> remote */
        int slot = 0, user = 0;
        char kind[16] = "";
        sscanf(arg, "%d %15s %d", &slot, kind, &user);
        if (slot < 0 || slot > 1) {
            snprintf(out, size, "usage: src <slot 0|1> pad <xinput user> | src <slot> remote");
            return 1;
        }
        if (!strcmp(kind, "remote"))
            src[slot].kind = R1_SRC_REMOTE;
        else
            src[slot].kind = R1_SRC_PAD, src[slot].arg = user;
        snprintf(out, size, "input source of slot %d: %s %d (records received: %u)", slot, src[slot].kind == R1_SRC_REMOTE ? "remote record" : "local pad of XInput user", src[slot].arg, src[slot].seq);
        return 1;
    }
    if (!strcmp(name, "rec")) {   /* rec <slot> <buttons hex> <lt> <rt> <lx> <ly> <rx> <ry>: simulates the network layer handing in a record (needs `src <slot> remote`) */
        int slot = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        char b[16] = "0";
        sscanf(arg, "%d %15s %d %d %d %d %d %d", &slot, b, &lt, &rt, &lx, &ly, &rx, &ry);
        r1_input_t r = {(uint16_t)hx(b), (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry};
        input_remote_set(slot, &r);
        snprintf(out, size, "record #%u for slot %d: buttons %04x lt %d rt %d L(%d,%d) R(%d,%d)%s", src[slot < 0 || slot > 1 ? 0 : slot].seq, slot, r.buttons, r.lt, r.rt, r.lx, r.ly, r.rx, r.ry,
                 src[slot < 0 || slot > 1 ? 0 : slot].kind == R1_SRC_REMOTE ? " (published)" : " (source is not remote: stored only)");
        return 1;
    }
    if (!strcmp(name, "shot")) {
        if (A->version >= 2 && A->shot)
            A->shot();
        snprintf(out, size, "shot requested");
        return 1;
    }
    return 0;
}

static void on_exit_(void)
{
    char tmp[600];
    if (role.armed || caps[0].active || caps[1].active || caps[2].active || caps[3].active)
        cap_off_all(tmp, sizeof tmp);
    padfix = 0;
    dbg_cleanup();
    hook_remove(&hk_pad);
    hook_remove(&hk_upd);
    hook_remove(&hk_pd);
    A->log("plug_cap: unloaded, hooks removed");
}

__declspec(dllexport) int plug_init(const r1_api_t *api, r1_plug_t *out)
{
    A = api;
    static const unsigned char b_pad[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    static const unsigned char b_upd[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54};
    static const unsigned char b_pd[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    if (hook_install(&hk_pad, 0x6AFBD0, b_pad, 6, (void *)hook_updatePad))
        orig_updatePad = (void (FC *)(unsigned))hk_pad.tramp;
    if (hook_install(&hk_upd, 0x6CDD70, b_upd, 9, (void *)hook_unitUpdate))
        orig_unitUpdate = (void (FC *)(unsigned))hk_upd.tramp;
    if (hook_install(&hk_pd, 0x88A470, b_pd, 6, (void *)hook_padData))
        orig_padData = (void (TC *)(void *, unsigned))hk_pd.tramp;
    A->log("plug_cap: loaded (api v%u), hooks updatePad %d unitUpdate %d padData %d", A->version, hk_pad.on, hk_upd.on, hk_pd.on);
    src[0].kind = R1_SRC_PAD, src[0].arg = 0;
    src[1].kind = R1_SRC_PAD, src[1].arg = 1;
    if (!hk_pd.on || !hk_pad.on || !hk_upd.on) {
        on_exit_();
        return 1;
    }
    out->on_cmd = on_cmd;
    out->on_frame = on_frame_;
    out->on_exit = on_exit_;
    return 0;
}
