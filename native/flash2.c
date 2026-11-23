/*
 * flash2.c - the second human's flashlight in split screen (Revelations 1), branch fix026f. Study and live evidence: notes\split-flashlight.md.
 *
 * FACTS (Ghidra, work\ghidra, sGameLights = [[0x116A7D4] + 0x44]):
 *   - the game has ONE torch: a uGameFlashSpot (+0xCCF0) + a uGameFlashPoint (+0xD0F0) inside sGameLights. It is on in a zone that has flash-light data (+0xF224 = 1, +0xF225 the
 *     mode last applied) and follows ONE actress, [sGameLights+0xF220] (the own human). FUN_008616f0 (called by sGameLights vf19, the per-frame update) writes the torch's world
 *     position / direction into the object at +0xCF20 / +0xCF30 (and the phase at +0xCF78): from the camera manager of viewport [0x116C3E4] when it is in a play state, else from
 *     the actress's joint 2; the aim is smoothed by the spring state +0xF230..+0xF23C.
 *   - the second human therefore never had a torch (a partner in the dark is a black silhouette without a beam).
 * (live 2026-11-22: writing the block at +0xCF20 per viewport changed nothing in the picture: the renderer reads the light's DOUBLE-BUFFERED RECORD that uSpotLight_vf9 (0xAA1A40) builds from
 *  the object's +0x230 / +0x240 each frame (records at +0x60 + idx*0xC0, idx = bit 15 of +0x30). So the spot's own vf9 is wrapped too: it is run a second time with the second human's block and both records are kept.)
 * MODEL: the update of vf19 is run a second time for the second human (actress +0xF220 = his unit, viewport index 2 = manager 1, his own spring state) right after the stock one;
 *   the two results (pos / dir / phase block at +0xCF20, 0x60 bytes) are kept as snapshots. The D3D SetViewport hook (d3d.c) writes the second human's block into the torch
 *   while the second viewport draws and the first human's block for every other viewport (the first half, the composite). Nothing is patched in code: the sGameLights object
 *   gets a private vtable copy whose slot 19 is wrapped.
 * Switch: console `split flash 0|1` (default 1), env REV1_FLASH2=0 at start.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"
#include "flash2.h"

#define FC __attribute__((fastcall))
#define HOLDER_PTR 0x116A7D4u
#define LIGHTS_OFF 0x44u
#define LIGHTS_VTBL 0x108EB38u
#define LIGHTS_VF19 0x863F20u
#define FN_FLASH_UPDATE 0x8616F0u   /* void __fastcall (sGameLights *) */
#define VIEW_IDX_PTR 0x116C3E4u
#define OFF_ACTRESS 0xF220u
#define OFF_MODE 0xF224u
#define OFF_MODE_APPLIED 0xF225u
#define OFF_SPRING 0xF230u
#define OFF_SPOT_FLAGS 0xCCFCu
#define OFF_BLOCK 0xCF20u
#define BLOCK_BYTES 0x60u
#define VT_SLOTS 24

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((void *)(size_t)a, &v, 4);
    return v;
}
static int wr(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)(size_t)a, p, n, &got) && got == n;
}
static int wr32(unsigned a, unsigned v) { return wr(a, &v, 4); }
static int rdn(unsigned a, void *p, unsigned n) { return r1_safe_read((void *)(size_t)a, p, n); }

static volatile int f_on = 0, f_env_done;   /* build 12: OFF by default (the picture proof failed, F23R KILL missed with it on); env REV1_FLASH2=1 or console `split flash 1` turns it on */
static volatile int f_dark;   /* LAB `split flash 4|5`: the scene's pooled lights switched off every pass (flags bits 0xC00), so the torch is the only light: 4 = torch on, 5 = torch off too (picture of the beam) */
static volatile int f_diag;   /* LAB `split flash 2|3`: 2 = the first human's view gets a torch moved 100000 units away, 3 = the second human's (does the per-viewport write reach the picture?) */
static unsigned *vt_copy_store, vt_for;
static void (FC *orig_vf19)(unsigned);
static unsigned char snap1[BLOCK_BYTES], snap2[BLOCK_BYTES];   /* the torch block of the first / second human, valid while snap_ok */
static volatile unsigned tr_skip, call_log, f_boost;   /* tr_skip: mode 9 ignores the proxy update function itself; call_log: LAB `split flash 11` logs the next 60 vf19 / vf9 calls (thread, viewport global, flags) */
#define SPOT_OFF 0xCCF0u
#define SPOT_VTBL 0x108EF28u
#define SPOT_VF9 0xAA1A40u
#define REC_LO 0x60u
#define REC_HI 0x1D0u   /* the records and matrices of both buffers */
#define SAVE_LO 0x30u
#define SAVE_HI 0x300u  /* everything vf9 may touch: restored after the second run */
static unsigned char rec1[REC_HI - REC_LO], rec2[REC_HI - REC_LO];
/* build-time swap (build 11): uLight_vf18 (0xA9B720) binds the ADDRESS of the render record (light + bit15*0xC0 + 0x60) into the draw context (ctx + 0x1F0 + (DAT_010C91FC[slot] & 0xFFF)*8);
 * the draw context's viewport index is the byte at ctx+0x158 (split: 0 = first half, 2 = second half). For the second half the bound address is replaced by a private 0xC0-byte copy
 * (double-buffered with the same bit 15 the engine uses) of the second human's record. */
#define SPOT_VF18 0xA9B720u
#define TAB_REC_SLOT 0x10C91FCu
#define VP_SECOND_IDX 2u
static unsigned char recbuf[2][0xC0] __attribute__((aligned(16)));
static volatile int recbuf_ok[2];
static void (FC *orig_spot18)(unsigned, unsigned, unsigned, unsigned);
static volatile unsigned f_boost2;        /* LAB `split flash 17`: the second half's record colour x50, range x10 (does the swapped record reach the picture?); 18: the same for the first human's real record */
static struct { unsigned v18[8], swapped, unexpected; unsigned ctx_seen[4]; } V;
static volatile int rec_ok;
static unsigned *spot_vt_store, spot_vt_for, spot_now;
static void (FC *orig_spot9)(unsigned);
static unsigned char spring2[16], spring2_ok;                   /* the second human's own spring state (+0xF230..+0xF23C) */
static volatile int snap_ok;                                    /* both snapshots made this frame */
static volatile unsigned p2_unit_now, lights_now;
static struct { unsigned spot9, runs, skipped, pass1, pass2, installs; unsigned lines; } S;
static DWORD last_active;

static unsigned lights_addr(void)
{
    unsigned h = rd32(HOLDER_PTR);
    return h ? rd32(h + LIGHTS_OFF) : 0;
}

/* second run of the stock torch update for the second human (game thread, inside the wrapped vf19, after the stock run) */
static void second_run(unsigned sl)
{
    unsigned unit = p2_unit_now;
    unsigned mode = rd32(sl + OFF_MODE) & 0xFFFFu;   /* bytes +0xF224 (mode) and +0xF225 (applied) */
    unsigned spot = rd32(sl + OFF_SPOT_FLAGS);
    if (!unit || (mode & 0xFF) == 0 || ((mode >> 8) & 0xFF) != (mode & 0xFF) || (spot & 7) != 2 || (spot & 0x4800) != 0x4800) {
        S.skipped++;
        snap_ok = 0;
        return;
    }
    unsigned char save_block[BLOCK_BYTES], save_spring[16];
    unsigned save_actress = rd32(sl + OFF_ACTRESS), save_view = rd32(VIEW_IDX_PTR);
    if (!rdn(sl + OFF_BLOCK, save_block, BLOCK_BYTES) || !rdn(sl + OFF_SPRING, save_spring, 16) || !save_actress) {
        snap_ok = 0;
        return;
    }
    memcpy(snap1, save_block, BLOCK_BYTES);
    if (!spring2_ok)
        memcpy(spring2, save_spring, 16), spring2_ok = 1;
    wr32(sl + OFF_ACTRESS, unit);
    wr32(VIEW_IDX_PTR, 2);
    wr(sl + OFF_SPRING, spring2, 16);
    ((void(FC *)(unsigned))(size_t)FN_FLASH_UPDATE)(sl);
    rdn(sl + OFF_BLOCK, snap2, BLOCK_BYTES);
    rdn(sl + OFF_SPRING, spring2, 16);
    wr32(sl + OFF_ACTRESS, save_actress);
    wr32(VIEW_IDX_PTR, save_view);
    wr(sl + OFF_SPRING, save_spring, 16);
    wr(sl + OFF_BLOCK, save_block, BLOCK_BYTES);
    snap_ok = 1;
    S.runs++;
    if (S.lines < 6 && (S.runs == 1 || S.runs % 600 == 0)) {
        float *a = (float *)snap1, *b = (float *)snap2;
        r1_log("flash2: second torch run %u: first human pos (%.0f,%.0f,%.0f) dir (%.2f,%.2f,%.2f); second human (unit %08x) pos (%.0f,%.0f,%.0f) dir (%.2f,%.2f,%.2f)", S.runs, a[0], a[1], a[2], a[4], a[5],
               a[6], unit, b[0], b[1], b[2], b[4], b[5], b[6]);
        S.lines++;
    }
}

static void FC wrap_spot9(unsigned self)
{
    if (call_log) {
        call_log--;
        r1_log("flash2: CALL vf9 tid %lu view %u flags %08x", (unsigned long)GetCurrentThreadId(), rd32(VIEW_IDX_PTR), rd32(self + 0x30));
    }
    orig_spot9(self);
    if (f_boost && self == spot_now) {   /* LAB `split flash 15`: the record colour x20 at BUILD time (both buffers, every call), `16`: colour 0 */
        for (unsigned o = 0; o < 2; o++)
            for (unsigned k = 0; k < 3; k++) {
                float *c = (float *)(size_t)(self + 0x80 + o * 0xC0 + k * 4);
                *c = f_boost == 1 ? *c * 20.0f : 0.0f;
            }
    }
    if (f_boost2 == 2 && self == spot_now) {   /* LAB `split flash 18`: the FIRST human's real record boosted (both buffers) */
        for (unsigned o = 0; o < 2; o++) {
            float *f = (float *)(size_t)(self + REC_LO + o * 0xC0);
            f[8] *= 50.0f, f[9] *= 50.0f, f[10] *= 50.0f, f[12] *= 10.0f, f[13] *= 10.0f;
        }
    }
    if (!f_on || !snap_ok || self != spot_now) {
        rec_ok = 0;
        recbuf_ok[0] = recbuf_ok[1] = 0;
        return;
    }
    static unsigned char save[SAVE_HI - SAVE_LO];
    if (!rdn(self + SAVE_LO, save, sizeof save) || !rdn(self + REC_LO, rec1, sizeof rec1)) {
        rec_ok = 0;
        return;
    }
    wr(self + 0x230, snap2, BLOCK_BYTES);
    orig_spot9(self);
    rdn(self + REC_LO, rec2, sizeof rec2);
    {   /* the fresh record of the second human (buffer idx = bit 15 of the flags, the one the engine builds this frame) for the build-time swap in wrap_spot18 */
        unsigned idx = (rd32(self + 0x30) >> 15) & 1;
        unsigned char tmp[0xC0];
        if (rdn(self + REC_LO + idx * 0xC0, tmp, 0xC0)) {
            float *f = (float *)tmp;
            if (f_diag == 3)
                f[1] += 100000.0f;
            if (f_diag == 6 || f_boost2 == 1)
                f[8] *= f_boost2 ? 50.0f : 20.0f, f[9] *= f_boost2 ? 50.0f : 20.0f, f[10] *= f_boost2 ? 50.0f : 20.0f;
            if (f_boost2 == 1)
                f[12] *= 10.0f, f[13] *= 10.0f;
            memcpy(recbuf[idx], tmp, 0xC0);
            recbuf_ok[idx] = 1;
        }
    }
    wr(self + SAVE_LO, save, sizeof save);
    rec_ok = 1;
    S.spot9++;
}

static void FC wrap_spot18(unsigned self, unsigned edx, unsigned ctx, unsigned slot)
{
    orig_spot18(self, edx, ctx, slot);
    if (self != spot_now)
        return;
    unsigned vp = rd32(ctx + 0x158) & 0xFF;
    V.v18[vp & 7]++;
    if (vp != VP_SECOND_IDX || !f_on || slot > 0x7F)
        return;
    unsigned idx = (rd32(self + 0x30) >> 15) & 1;
    if (!recbuf_ok[idx])
        return;
    unsigned a = ctx + 0x1F0 + (rd32(TAB_REC_SLOT + slot * 4) & 0xFFF) * 8, cur = rd32(a);
    if ((cur & 0xFFFFFFF0u) != self + REC_LO + idx * 0xC0) {
        V.unexpected++;
        return;
    }
    wr32(a, (unsigned)(size_t)recbuf[idx] | (cur & 0xF));
    wr32(ctx + 0x144, rd32(ctx + 0x144) | 1);
    V.swapped++;
    if (V.swapped == 1)
        r1_log("flash2: build-time swap works: ctx %08x slot %u bound record %08x -> private copy %08x (viewport %u)", ctx, slot, cur, (unsigned)(size_t)recbuf[idx], vp);
}

static void FC wrap_vf19(unsigned self)
{
    if (call_log) {
        call_log--;
        r1_log("flash2: CALL vf19 tid %lu view %u", (unsigned long)GetCurrentThreadId(), rd32(VIEW_IDX_PTR));
    }
    orig_vf19(self);
    if (f_on && p2_unit_now && self == lights_now)
        second_run(self);
    else
        snap_ok = 0;
}

static int install(unsigned sl)
{
    unsigned vt = rd32(sl);
    if (vt != LIGHTS_VTBL && vt != vt_for) {
        r1_log("flash2: sGameLights %08x has vtable %08x, expected %08x: off", sl, vt, LIGHTS_VTBL);
        return 0;
    }
    if (vt == LIGHTS_VTBL) {
        if (rd32(vt + 19 * 4) != LIGHTS_VF19) {
            r1_log("flash2: vf19 of the sGameLights vtable is %08x, expected %08x: off", rd32(vt + 19 * 4), LIGHTS_VF19);
            return 0;
        }
        if (!vt_copy_store) {
            unsigned *c = VirtualAlloc(NULL, VT_SLOTS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!c)
                return 0;
            for (int i = 0; i < VT_SLOTS; i++)
                c[i] = rd32(vt + 4u * (unsigned)i);
            orig_vf19 = (void (FC *)(unsigned))(size_t)c[19];
            c[19] = (unsigned)(size_t)wrap_vf19;
            vt_copy_store = c;
        }
        vt_for = (unsigned)(size_t)vt_copy_store;
        if (!wr32(sl, vt_for))
            return 0;
        S.installs++;
        r1_log("flash2: sGameLights %08x: vf19 wrapped (second torch run for the second human)", sl);
    }
    {   /* the torch's own vf9 (builds the render record) */
        unsigned spot = sl + SPOT_OFF, svt = rd32(spot);
        if (svt == SPOT_VTBL) {
            if (rd32(svt + 9 * 4) != SPOT_VF9) {
                r1_log("flash2: the spot's vf9 is %08x, expected %08x: the render record is not swapped", rd32(svt + 9 * 4), SPOT_VF9);
            } else {
                if (!spot_vt_store) {
                    unsigned *c = VirtualAlloc(NULL, VT_SLOTS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (c) {
                        for (int i = 0; i < VT_SLOTS; i++)
                            c[i] = rd32(svt + 4u * (unsigned)i);
                        orig_spot9 = (void (FC *)(unsigned))(size_t)c[9];
                        c[9] = (unsigned)(size_t)wrap_spot9;
                        if (c[18] == SPOT_VF18) {
                            orig_spot18 = (void (FC *)(unsigned, unsigned, unsigned, unsigned))(size_t)c[18];
                            c[18] = (unsigned)(size_t)wrap_spot18;
                        } else
                            r1_log("flash2: the spot's vf18 is %08x, expected %08x: no build-time swap", c[18], SPOT_VF18);
                        spot_vt_store = c;
                    }
                }
                if (spot_vt_store && wr32(spot, (unsigned)(size_t)spot_vt_store)) {
                    spot_vt_for = (unsigned)(size_t)spot_vt_store;
                    r1_log("flash2: the torch spot %08x: vf9 wrapped (second render record)", spot);
                }
            }
        } else if (svt != spot_vt_for)
            r1_log("flash2: the spot %08x has vtable %08x, expected %08x", spot, svt, SPOT_VTBL);
    }
    return 1;
}

static void lights_off(unsigned sl)
{
    static const struct { unsigned off, n, stride; } pool[] = {{0x4BFC, 8, 800}, {0x64FC, 8, 0x360}, {0x7FFC, 16, 800}, {0xB1FC, 8, 0x360}, {0x1EC, 13, 0x300}, {0x28EC, 11, 0x330},
                                                              {0xD7CC, 1, 0}, {0xD9DC, 1, 0}, {0xE5AC, 2, 800}, {0xEBEC, 2, 800}};
    for (unsigned k = 0; k < sizeof pool / sizeof pool[0]; k++)
        for (unsigned i = 0; i < pool[k].n; i++) {
            unsigned a = sl + pool[k].off + i * pool[k].stride;
            wr32(a, rd32(a) & ~0xC00u);
        }
}


/* ---- LAB `split flash 8|9|10`: who reads the torch spot's render record (8) / its light proxy [spot+0x224] (9)? PAGE_GUARD on the pages, a vectored handler logs (eip, access, thread,
 * return addresses found on the stack) of every distinct access and single-steps so the page is guarded again. 10 = off (also off by itself after 8 s). */
#define TR_MAX 400
static struct { unsigned eip, off, tid, rw, ret[5]; } tr_ev[TR_MAX];
static volatile LONG tr_n;
static volatile unsigned tr_lo, tr_hi, tr_base, tr_pg[4], tr_npg, tr_armed, tr_printed;
static DWORD tr_t0, tr_render_tid;
static struct { DWORD tid; unsigned page; } tr_re[8];
static PVOID tr_veh;

static LONG CALLBACK tr_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode, tid = GetCurrentThreadId();
    if (code == 0x80000001u && ep->ExceptionRecord->NumberParameters >= 2) {   /* STATUS_GUARD_PAGE_VIOLATION */
        unsigned a = (unsigned)ep->ExceptionRecord->ExceptionInformation[1], rw = (unsigned)ep->ExceptionRecord->ExceptionInformation[0], pg = a & ~0xFFFu, hit = 0;
        for (unsigned i = 0; i < tr_npg; i++)
            hit |= tr_pg[i] == pg;
        if (!hit)
            return EXCEPTION_CONTINUE_SEARCH;
        unsigned eip = (unsigned)(size_t)ep->ExceptionRecord->ExceptionAddress;
        if (tr_skip == 2 && ((eip >= 0xAA1A40 && eip < 0xAA2CD0) || (eip >= 0x8616F0 && eip < 0x862250) || eip >= 0x1200000))
            goto rearm;
        if (tr_skip == 1 && ((eip >= 0xd83f50 && eip < 0xd85000) || (eip >= 0xc25d00 && eip < 0xc26000)))
            goto rearm;
        if (tr_armed && a >= tr_lo && a < tr_hi) {
            LONG n = tr_n;
            int dup = 0;
            for (LONG i = 0; i < n && i < TR_MAX; i++)
                dup |= tr_ev[i].eip == eip && tr_ev[i].rw == rw && tr_ev[i].tid == tid;
            if (!dup && n < TR_MAX) {
                unsigned k = 0, esp = ep->ContextRecord->Esp, v;
                tr_ev[n].eip = eip, tr_ev[n].rw = rw, tr_ev[n].tid = tid, tr_ev[n].off = a - tr_base;
                for (unsigned i = 0; i < 48 && k < 5; i++) {
                    unsigned char b[5];
                    if (!r1_safe_read((void *)(size_t)(esp + i * 4), &v, 4) || v < 0x401005 || v > 0x1200000)
                        continue;
                    if (r1_safe_read((void *)(size_t)(v - 5), b, 5) && (b[0] == 0xE8 || b[3] == 0xFF || b[4] == 0xFF || b[2] == 0xFF))
                        tr_ev[n].ret[k++] = v;
                }
                tr_n = n + 1;
            }
        }
    rearm:
        for (int i = 0; i < 8; i++)
            if (!tr_re[i].tid) {
                tr_re[i].page = pg, tr_re[i].tid = tid;
                break;
            }
        ep->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        for (int i = 0; i < 8; i++)
            if (tr_re[i].tid == tid) {
                DWORD old;
                if (tr_armed)
                    VirtualProtect((void *)(size_t)tr_re[i].page, 1, PAGE_READWRITE | PAGE_GUARD, &old);
                tr_re[i].tid = 0;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void tr_arm(unsigned lo, unsigned hi, unsigned base)
{
    if (!tr_veh)
        tr_veh = AddVectoredExceptionHandler(1, tr_handler);
    tr_n = 0, tr_printed = 0;
    tr_lo = lo, tr_hi = hi, tr_base = base;
    tr_npg = 0;
    for (unsigned pg = lo & ~0xFFFu; pg < hi && tr_npg < 4; pg += 0x1000)
        tr_pg[tr_npg++] = pg;
    tr_armed = 1;
    tr_t0 = GetTickCount();
    for (unsigned i = 0; i < tr_npg; i++) {
        DWORD old;
        VirtualProtect((void *)(size_t)tr_pg[i], 1, PAGE_READWRITE | PAGE_GUARD, &old);
    }
    r1_log("flash2: TRACE armed on %08x..%08x (%u page(s), base %08x), render thread %lu", lo, hi, tr_npg, base, (unsigned long)tr_render_tid);
}
static void tr_disarm(void)
{
    if (!tr_armed)
        return;
    tr_armed = 0;
    for (unsigned i = 0; i < tr_npg; i++) {
        DWORD old;
        VirtualProtect((void *)(size_t)tr_pg[i], 1, PAGE_READWRITE, &old);
    }
    r1_log("flash2: TRACE off, %ld distinct accesses", (long)tr_n);
}
static void tr_print(void)
{
    LONG n = tr_n;
    for (; tr_printed < (unsigned)n && tr_printed < TR_MAX; tr_printed++) {
        unsigned i = tr_printed;
        r1_log("flash2: TRACE %s off %03x eip %08x thread %lu%s callers %08x %08x %08x %08x %08x", tr_ev[i].rw ? "WRITE" : "read ", tr_ev[i].off, tr_ev[i].eip, (unsigned long)tr_ev[i].tid,
               tr_ev[i].tid == tr_render_tid ? " (RENDER)" : "", tr_ev[i].ret[0], tr_ev[i].ret[1], tr_ev[i].ret[2], tr_ev[i].ret[3], tr_ev[i].ret[4]);
    }
    if (tr_armed && GetTickCount() - tr_t0 > 8000)
        tr_disarm();
}

void flash2_frame(int active, unsigned p2_unit)
{
    tr_print();
    if (!f_env_done) {
        const char *e = getenv("REV1_FLASH2");
        f_env_done = 1;
        if (e && e[0] == '1')
            f_on = 1;
    }
    unsigned sl = lights_addr();
    if (sl && f_dark)
        lights_off(sl);
    if (!f_on && !vt_copy_store) {   /* off and never installed: touch nothing */
        p2_unit_now = 0;
        snap_ok = 0;
        return;
    }
    if (!active || !p2_unit || !sl) {
        p2_unit_now = 0;
        snap_ok = 0;
        if (GetTickCount() - last_active > 1500)
            spring2_ok = 0;   /* (the pass calls this with 0 first and again with the unit later: only a real pause drops the spring state) */
        return;
    }
    last_active = GetTickCount();
    if (rd32(sl) != vt_for && !install(sl)) {
        p2_unit_now = 0;
        return;
    }
    lights_now = sl;
    spot_now = sl + SPOT_OFF;
    p2_unit_now = f_on ? p2_unit : 0;
}

/* render thread: SetViewport of the device. (x, y, w, h) the new viewport, (bw, bh) the back buffer. */
void flash2_viewport(unsigned x, unsigned y, unsigned w, unsigned h, unsigned bw, unsigned bh)
{
    tr_render_tid = GetCurrentThreadId();
    unsigned spot = spot_now;
    if (1 || !rec_ok || !snap_ok || !spot || !p2_unit_now || !f_on)   /* build 11: the per-viewport write of the record is replaced by the build-time swap (wrap_spot18); the old way is kept below, unreachable */
        return;
    int second = (x > 0 || y > 0) && (w < bw || h < bh) && w * 2 + 8 >= bw && h * 2 + 8 >= bh;
    unsigned char tmp[REC_HI - REC_LO];
    const unsigned char *src = second ? rec2 : rec1;
    if (f_diag == (second ? 3 : 2)) {   /* LAB: this view's torch is taken away (its record's position pushed 100000 units up) */
        memcpy(tmp, src, sizeof tmp);
        for (unsigned o = 0; o < 2; o++)
            ((float *)(tmp + o * 0xC0))[1] += 100000.0f;
        src = tmp;
    }
    if (f_diag == (second ? 6 : 7)) {   /* LAB: this view's torch colour x20 (record colour at +0x20 of each buffer): does the record reach the picture? */
        memcpy(tmp, src, sizeof tmp);
        for (unsigned o = 0; o < 2; o++)
            for (unsigned k = 8; k < 11; k++)
                ((float *)(tmp + o * 0xC0))[k] *= 20.0f;
        src = tmp;
    }
    wr(spot + REC_LO, src, REC_HI - REC_LO);
    if (second)
        S.pass2++;
    else
        S.pass1++;
}

void flash2_set(int on)
{
    f_boost = on == 15 ? 1 : on == 16 ? 2 : 0;
    f_boost2 = on == 17 ? 1 : on == 18 ? 2 : 0;
    tr_skip = on == 9 ? 1 : (on == 13 || on == 14) ? 2 : 0;
    if (on == 11)
        call_log = 60;
    if (on == 13 && spot_now)
        tr_arm(spot_now + 0x1D0, spot_now + 0x230, spot_now);
    else if (on == 14 && lights_now)
        tr_arm(lights_now + 0xCF20, lights_now + 0xCF80, lights_now);
    else if (on == 12 && spot_now)
        tr_arm(spot_now + 0x2A8, spot_now + 0x300, spot_now);
    else if (on == 8 && spot_now)
        tr_arm(spot_now + REC_LO, spot_now + REC_HI, spot_now);
    else if (on == 9 && spot_now && rd32(spot_now + 0x224))
        tr_arm(rd32(spot_now + 0x224), rd32(spot_now + 0x224) + 0x120, rd32(spot_now + 0x224));
    else if (on == 10 || on < 8)
        tr_disarm();
    if (on >= 8)
        on = 1;
    f_on = on != 0 && on != 5;
    f_diag = on == 2 || on == 3 || on == 6 || on == 7 ? on : 0;
    f_dark = on == 4 || on == 5;
}
int flash2_is_on(void) { return f_on; }
void flash2_status(char *out, int size)
{
    float *c1 = (float *)(rec1 + 0x20), *c2 = (float *)(rec2 + 0x20), *p1 = (float *)rec1, *p2 = (float *)rec2;
    unsigned n = (unsigned)snprintf(out, (size_t)size, "flash2: %s; lights %08x second human %08x; vtable %s; runs %u skipped %u, spot vf9 runs %u, viewport writes first %u second %u, snapshots %s records %s", f_on ? (f_diag ? "on (DIAG: 2/3 torch moved away, 6/7 colour x20)" : "on") : "off", lights_now, p2_unit_now,
             vt_copy_store ? "wrapped" : "stock", S.runs, S.skipped, S.spot9, S.pass1, S.pass2, snap_ok ? "valid" : "none", rec_ok ? "valid" : "none");
    if (n < (unsigned)size)
        n += (unsigned)snprintf(out + n, (size_t)size - n, "; vf18 calls by viewport v0 %u v1 %u v2 %u v3 %u, swapped %u, unexpected %u, bufs %d/%d boost2 %u; ", V.v18[0], V.v18[1], V.v18[2], V.v18[3], V.swapped, V.unexpected, recbuf_ok[0], recbuf_ok[1], f_boost2);
    if (n < (unsigned)size)
        snprintf(out + n, (size_t)size - n, "; record 1 pos (%.0f,%.0f,%.0f) colour (%.3f,%.3f,%.3f) range %.1f/%.1f; record 2 pos (%.0f,%.0f,%.0f) colour (%.3f,%.3f,%.3f); render thread %lu", p1[0], p1[1], p1[2], c1[0], c1[1], c1[2], p1[12], p1[13], p2[0], p2[1], p2[2], c2[0], c2[1],
                 c2[2], (unsigned long)tr_render_tid);
}
