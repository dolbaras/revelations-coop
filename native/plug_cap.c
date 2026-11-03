/*
 * Hot-loadable plug-in: partner capture experiments (stage 3). Loaded with the console "plug load <path>\r1plug_N.dll", commands "P cap ...".
 * It installs its own inline hooks and removes them when unloaded, so the capture logic can change without restarting the game.
 *
 * Findings it builds on (notes\stage1-live.md):
 *   - uPlayer::updatePad (0x6AFBD0) builds the unit's pad group (+0x1138..+0x1151) from the control type [[unit+0xE80]+0x38]+0x34:
 *     1 = input manager, 2 = AI virtual pad (think+0x50..), 3 = remote (net fields).
 *   - the input manager's readers use the "current pad slot" sGamePad+0x870 (always 0 in play); the per-slot PadData (sGamePad+0x628+0x90*k) is built
 *     by FUN_0088a470(this = &PadData[k], k) every frame, but that function reads the raw pad of the CURRENT slot (+0x870), not of slot k:
 *     both PadData slots mirror pad 0. "padfix on" makes it read pad k for slot k (+0x870 = k during the call).
 *   - "cap on <unit> <slot> 2": the unit gets control type 1 and kind 1 and its whole frame update (0x6CDD70, which includes updatePad and every
 *     button reader of the state machine) runs with +0x870 = slot, i.e. it reads pad <slot> through the game's own code.
 *   - "cap on <unit> <slot> 1": control type 1, the pad group is written after updatePad from the XInput state of user <slot> (our own scaling): the path
 *     a network source would use (PR #115 recipe).
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "r1.h"

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

/* ---- state ---- */
#define GAMEPAD_PTR 0x11E0388u

static hk_t hk_pad, hk_upd, hk_pd;
static void (FC *orig_updatePad)(unsigned unit);
static void (FC *orig_unitUpdate)(unsigned unit);
static void (TC *orig_padData)(void *self, unsigned slot);

static struct {
    volatile int on;
    unsigned unit, ctl_obj;
    int slot, mode, saved_ctl, saved_kind;
    unsigned swaps, writes;
    float dz;
} cap = {.dz = 0.24f};
static volatile int padfix;
static unsigned padfix_calls;

static unsigned gp(void) { return rd32(GAMEPAD_PTR); }

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

static void swap_call(void (FC *fn)(unsigned), unsigned unit)
{
    unsigned g = gp();
    if (!g) {
        fn(unit);
        return;
    }
    unsigned old = rd32(g + 0x870);
    wr32(g + 0x870, (unsigned)cap.slot);
    fn(unit);
    wr32(g + 0x870, old);
    cap.swaps++;
}

static float dead(float v, float dz)
{
    float a = fabsf(v);
    if (a < dz)
        return 0;
    float r = (a - dz) / (1.0f - dz);
    return v < 0 ? -r : r;
}

static void write_group(unsigned unit)
{
    XINPUT_GAMEPAD pad;
    memset(&pad, 0, sizeof pad);
    A->pad_peek(cap.slot, &pad);
    float lx = pad.sThumbLX / 32767.0f, ly = pad.sThumbLY / 32767.0f, rx = pad.sThumbRX / 32767.0f, ry = pad.sThumbRY / 32767.0f;
    float mag = sqrtf(lx * lx + ly * ly);
    float mv_x = 0, mv_y = 0;
    if (mag > cap.dz) {
        float k = (mag - cap.dz) / (1.0f - cap.dz) / mag;
        mv_x = lx * k, mv_y = ly * k;
    }
    wrf(unit + 0x1138, mv_x), wrf(unit + 0x113C, mv_y);
    wrf(unit + 0x1140, dead(rx, cap.dz)), wrf(unit + 0x1144, dead(ry, cap.dz));
    wrf(unit + 0x1148, dead(rx, cap.dz)), wrf(unit + 0x114C, dead(ry, cap.dz));
    /* run: the game's own "may run" test, and the stick pushed far */
    unsigned char may_run = ((unsigned char (FC *)(unsigned))0x6AD9E0)(unit);
    wr8(unit + 0x1150, may_run && mag > 0.7f);
    /* aim: weapon allows aiming (FUN_006ad740) and the left trigger is held */
    unsigned char may_aim = ((unsigned char (TC *)(unsigned, unsigned))0x6AD740)(unit, 0);   /* __thiscall with one stack argument (0), `ret 4`: updatePad calls it as `push 0; mov ecx,unit` */
    wr8(unit + 0x1151, may_aim && pad.bLeftTrigger > 100);
    cap.writes++;
}

static void FC hook_updatePad(unsigned unit)
{
    if (cap.on && unit == cap.unit) {
        if (cap.mode == 1) {
            orig_updatePad(unit);
            write_group(unit);
            return;
        }
        if (cap.mode == 0) {
            swap_call(orig_updatePad, unit);
            return;
        }
    }
    orig_updatePad(unit);
}

static void FC hook_unitUpdate(unsigned unit)
{
    if (cap.on && cap.mode == 2 && unit == cap.unit) {
        swap_call(orig_unitUpdate, unit);
        return;
    }
    orig_unitUpdate(unit);
}

/* ---- units ---- */
static unsigned ctl_obj_of(unsigned unit) { return rd32(rd32(unit + 0xE80) + 0x38); }

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

static void cap_off(char *out, int size)
{
    if (cap.on && cap.ctl_obj) {
        cap.on = 0;
        wr32(cap.ctl_obj + 0x34, (unsigned)cap.saved_ctl);
        wr32(cap.unit + 0xE30, (unsigned)cap.saved_kind);
        snprintf(out, size, "capture OFF: %08x control type back to %d, kind %d (%u swaps, %u writes)", cap.unit, cap.saved_ctl, cap.saved_kind, cap.swaps, cap.writes);
    } else
        snprintf(out, size, "capture was off");
}

static int on_cmd(const char *name, const char *arg, char *out, int size)
{
    if (!strcmp(name, "cap")) {
        char sub[16] = "", who[40] = "";
        int slot = 1, mode = 2;
        sscanf(arg, "%15s %39s %d %d", sub, who, &slot, &mode);
        if (!strcmp(sub, "on")) {
            unsigned unit = find_unit(who);
            char cn[96];
            if (cap.on) {
                snprintf(out, size, "capture already on for %08x (cap off first)", cap.unit);
                return 1;
            }
            if (!unit || !A->object_name(unit, cn, sizeof cn) || !ctl_obj_of(unit)) {
                snprintf(out, size, "no such unit '%s' (cap on partner|main|Parker|Jill|<hexaddr> [padslot=1] [mode 2 swap whole update|0 swap updatePad|1 write group])", who);
                return 1;
            }
            cap.unit = unit, cap.slot = slot, cap.mode = mode, cap.swaps = cap.writes = 0;
            cap.ctl_obj = ctl_obj_of(unit);
            cap.saved_ctl = (int)rd32(cap.ctl_obj + 0x34), cap.saved_kind = (int)rd32(unit + 0xE30);
            wr32(cap.ctl_obj + 0x34, 1);
            wr32(unit + 0xE30, 1);
            cap.on = 1;
            snprintf(out, size, "capture ON: %s %08x pad slot %d mode %d: control type %d -> 1, kind %d -> 1 (padfix %s)", cn, unit, slot, mode, cap.saved_ctl, cap.saved_kind, padfix ? "on" : "off");
        } else if (!strcmp(sub, "off")) {
            cap_off(out, size);
        } else if (!strcmp(sub, "dz")) {
            cap.dz = (float)atof(who);
            snprintf(out, size, "dead zone %.2f", cap.dz);
        } else {
            snprintf(out, size, "capture %s: unit %08x slot %d mode %d ctlobj %08x saved ctl %d kind %d, %u swaps, %u group writes, padfix %s (%u calls), hooks: updatePad %d unitUpdate %d padData %d",
                     cap.on ? "ON" : "off", cap.unit, cap.slot, cap.mode, cap.ctl_obj, cap.saved_ctl, cap.saved_kind, cap.swaps, cap.writes, padfix ? "on" : "off", padfix_calls, hk_pad.on, hk_upd.on,
                     hk_pd.on);
        }
        return 1;
    }
    if (!strcmp(name, "padfix")) {
        padfix = !strcmp(arg, "on");
        snprintf(out, size, "padfix %s", padfix ? "on: PadData[k] is built from pad k" : "off");
        return 1;
    }
    return 0;
}

static void on_exit_(void)
{
    char tmp[200];
    if (cap.on)
        cap_off(tmp, sizeof tmp);
    padfix = 0;
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
    A->log("plug_cap: loaded, hooks updatePad %d unitUpdate %d padData %d", hk_pad.on, hk_upd.on, hk_pd.on);
    if (!hk_pd.on) {   /* (a main DLL that already hooks updatePad/unitUpdate itself, as in the very first test session, only needs padData) */
        on_exit_();
        return 1;
    }
    out->on_cmd = on_cmd;
    out->on_exit = on_exit_;
    return 0;
}
