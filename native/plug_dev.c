/*
 * plug_dev.c - the development plug-in: the capture engine (capture.c, the same source as the main DLL's module) AND the debug toolbox, as ONE hot-loadable DLL.
 *
 * Why: the main DLL cannot be rebuilt while the game runs, and a session must not be ended to test a change (another agent takes the hardware lock within a minute). capture.c is
 * written against five services of the main DLL (r1_log, r1_safe_read, classes_object_name, testmode_pad_set / testmode_pad_peek, lt_add_frame_cb); here they are mapped to the
 * plug-in API, so every change of the capture engine can be iterated live: build, `plug load`, test. The main DLL's own copy stays idle (no role bound, no hook) while this
 * plug-in is loaded - never bind roles in both.
 *
 *   sh mod/native/build_plug.sh <new name> plug_dev.c      -> build\<name>.dll
 *   console:  plug load <dll>;  P cap status | bind | unbind | own | src | rec | recseq | trk | timeout | latch | raid ... (same arguments as the main DLL's `cap`),
 *             P mon ..., P thr <ms>, P ulist [filter], P snap/diff/find/ptrs/bp/dr (plug_dbg.inc), P shot
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "r1.h"
#include "capture.h"

static const r1_api_t *A;

/* ---- the main DLL's services, mapped to the plug-in API ---- */
void r1_log(const char *fmt, ...)
{
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    A->log("%s", b);
}
int r1_safe_read(const void *addr, void *out, unsigned size) { return A->safe_read(addr, out, size); }
int classes_object_name(unsigned obj, char *out, int size) { return A->object_name(obj, out, size); }
void testmode_pad_set(int slot, int active, const void *g) { A->pad_set(slot, active, g); }
int testmode_pad_peek(int user, void *out) { return A->pad_peek(user, out); }
static lt_frame_cb frame_cb;
void lt_add_frame_cb(lt_frame_cb cb) { frame_cb = cb; }

#include "capture.c"

/* ---- helpers of the debug includes ---- */
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
static unsigned hx(const char *s) { return (unsigned)strtoul(s, NULL, 16); }

/* unit by name for the snapshot tools: a hex address, a class name part (Jill, Parker), "partner" (kind 2) or "main" (kind 1) */
static unsigned find_unit(const char *who)
{
    unsigned sp = rd32(AD.splayer_ptr);
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
#include "plug_mon.inc"
#include "plug_thr.inc"

/* P ulist [filter]: every unit of the game (sUnit [0x11E0184]: `+0x620` groups, group k = first unit at +0x2C+0x18k, next unit at unit+0x14) whose class name contains the filter */
static void ulist_cmd(const char *filter, char *out, int size)
{
    unsigned su = rd32(0x11E0184), ng = su ? rd32(su + 0x620) : 0;
    int o = snprintf(out, size, "sUnit %08x, %u groups\n", su, ng), shown = 0;
    for (unsigned k = 0; k < ng && k < 40 && o + 160 < size; k++) {
        unsigned u = rd32(su + 0x2C + 0x18 * k);
        int n = 0, match = 0;
        for (int guard = 0; u && guard < 3000 && o + 160 < size; guard++, u = rd32(u + 0x14)) {
            char cn[96];
            n++;
            if (!A->object_name(u, cn, sizeof cn))
                strcpy(cn, "?");
            if (filter[0] && !strstr(cn, filter))
                continue;
            match++;
            if (shown < 90) {
                o += snprintf(out + o, size - o, "g%u %08x %-24s pos(%.0f,%.0f,%.0f) hp %.0f/%.0f state(F08) %d own(+30) %08x\n", k, u, cn, rdf(u + 0x40), rdf(u + 0x44), rdf(u + 0x48), rdf(u + 0xE38),
                              rdf(u + 0xE40), (int)rd32(u + 0xF08), rd32(u + 0x30));
                shown++;
            }
        }
        if (n)
            o += snprintf(out + o, size - o, "  group %u: %d units, %d match\n", k, n, match);
    }
}

static int on_cmd(const char *name, const char *arg, char *out, int size)
{
    if (dbg_cmd(name, arg, out, size))
        return 1;
    if (!strcmp(name, "mon"))
        return mon_cmd(arg, out, size);
    if (!strcmp(name, "thr"))
        return thr_cmd(arg, out, size);
    if (!strcmp(name, "cap")) {
        capture_cmd(arg, out, size);
        return 1;
    }
    if (!strcmp(name, "ulist")) {
        ulist_cmd(arg, out, size);
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

static void on_frame_(void)
{
    if (frame_cb)
        frame_cb();
    mon_frame();
    thr_frame();
}

static void on_exit_(void)
{
    thr_stop();
    capture_unbind(-1);
    capture_set_own(-1);
    capture_set_remote_timeout(0);
    for (int s = 0; s < 2; s++) {
        tk[s].state = 0;
        if (src[s].kind != CAP_NATIVE)
            capture_set_source(s, CAP_NATIVE);
    }
    if (cam_restore) {
        cam_restore = 0;
        cam_set_slot(0);
    }
    apply();   /* releases the taken characters and removes the hooks */
    dbg_cleanup();
    A->log("plug_dev: unloaded, hooks removed");
}

__declspec(dllexport) int plug_init(const r1_api_t *api, r1_plug_t *out)
{
    A = api;
    if (A->version < 3) {
        A->log("plug_dev: needs API v3");
        return 2;
    }
    if (A->capture_busy && A->capture_busy()) {
        A->log("plug_dev: the main DLL's capture module holds roles/hooks (`cap unbind all`, `cap own off` first)");
        return 3;
    }
    capture_init();
    A->log("plug_dev: loaded (capture engine + debug toolbox), commands: P cap ..., P mon ..., P thr, P ulist");
    out->on_cmd = on_cmd;
    out->on_frame = on_frame_;
    out->on_exit = on_exit_;
    return 0;
}
