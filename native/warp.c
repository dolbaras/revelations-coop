/*
 * warp.c - developer tool: start the campaign at ANOTHER section through the stock Continue path (hardware tests of enemies, doors, herbs, other character
 * pairs without playing through the story). Not part of the player's game: nothing happens unless the console command `warp` is used.
 *
 *   warp <hex scene id>   e.g. 91020100 = episode 2 section 1; the next Continue (Campaign -> Continue) starts that section instead of the saved one
 *   warp off | status
 *
 * Mechanism (stage 6b, static): the continue area aStartContinue (vtable 0xFF1994) runs its slot 7 (0x434F70) every frame until it succeeds; that function hands the
 * start request record [[0x116B3D4] + 0x60 + mode * 0x24] to the loader 0x6D0FF0, which reads the section's scene id at record +0x14 (0x91EESS00 = the area class
 * aEpisodeEE.. of the section) and makes the game load it. The wrapper overwrites that id before the original runs. The record is dumped to the log at the first frame.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "r1.h"

#define AC_VTABLE 0xFF1994u       /* aStartContinue */
#define AC_SLOT 7
#define AC_ORIG 0x434F70u
#define START_SINGLETON 0x116B3D4u
#define REC_BASE 0x60u
#define REC_SIZE 0x24u
#define REC_ID 0x14u

typedef void(__attribute__((fastcall)) * vf_t)(void *self);
static vf_t orig_vf;
static volatile unsigned g_id;       /* 0 = off */
static unsigned g_calls, g_writes, g_last_before, g_last_rec;
static int g_dumped;

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    return r1_safe_read((void *)(uintptr_t)a, &v, 4) ? v : 0;
}

static void __attribute__((fastcall)) hook_vf(void *self)
{
    g_calls++;
    unsigned obj = rd32(START_SINGLETON), holder = rd32(R1_GAMESYS_PTR);
    if (obj && holder) {
        unsigned g = rd32(holder + 0x24);
        unsigned mode = g ? rd32(g + 0x20) : 0;
        unsigned rec = obj + REC_BASE + (mode & 1) * REC_SIZE;
        unsigned cur = rd32(rec + REC_ID);
        g_last_rec = rec;
        if (!g_dumped) {
            g_dumped = 1;
            r1_log("warp: first frame of the continue area: singleton %08x mode %u record %08x id %08x; record bytes:", obj, mode, rec, cur);
            for (unsigned o = 0; o < REC_SIZE; o += 4)
                r1_log("warp:   +%02x %08x", o, rd32(rec + o));
        }
        if (g_id && cur && cur != g_id) {
            g_last_before = cur;
            DWORD old;
            if (VirtualProtect((void *)(uintptr_t)(rec + REC_ID), 4, PAGE_READWRITE, &old)) {
                *(volatile unsigned *)(uintptr_t)(rec + REC_ID) = g_id;
                g_writes++;
                if (g_writes <= 3)
                    r1_log("warp: scene id %08x replaced by %08x in the start record", cur, g_id);
            }
        }
    }
    orig_vf(self);
}

void warp_install(void)
{
    unsigned cur = rd32(AC_VTABLE + 4 * AC_SLOT);
    if (cur != AC_ORIG) {
        r1_log("warp: aStartContinue vtable slot %d is %08x, expected %08x: the warp tool stays off", AC_SLOT, cur, AC_ORIG);
        return;
    }
    void *fake = (void *)(uintptr_t)AC_VTABLE;
    orig_vf = (vf_t)(uintptr_t)cur;
    if (!r1_patch_vtable(&fake, AC_SLOT, (void *)hook_vf)) {
        r1_log("warp: cannot patch the vtable slot");
        return;
    }
    r1_log("warp: ready (console `warp <hex scene id>`)");
}

void warp_cmd(const char *arg, char *out, int size)
{
    if (!arg || !arg[0] || !strcmp(arg, "status")) {
        snprintf(out, size, "warp: %s scene id %08x; continue-area frames %u, writes %u (last replaced id %08x), last record %08x",
                 orig_vf ? "installed" : "NOT installed", g_id, g_calls, g_writes, g_last_before, g_last_rec);
        return;
    }
    if (!strcmp(arg, "off")) {
        g_id = 0;
        snprintf(out, size, "warp: off");
        return;
    }
    unsigned id = (unsigned)strtoul(arg, NULL, 16);
    if ((id & 0xFF000000u) != 0x91000000u) {
        snprintf(out, size, "warp: a scene id looks like 91EESS00 (hex), got '%s'", arg);
        return;
    }
    g_id = id;
    g_writes = 0;
    snprintf(out, size, "warp: the next Continue starts section %08x", id);
}
