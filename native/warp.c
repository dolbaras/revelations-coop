/*
 * warp.c - developer tool: start the campaign at ANOTHER section through the stock Continue path (hardware tests of enemies, doors, herbs, other character
 * pairs without playing through the story). Not part of the player's game: nothing is patched unless the console command `warp <id>` is used.
 *
 *   warp <hex id>   92020100 = the start of episode 2 section 1 (Continue asks for the area class 0x92EESS00 of the saved checkpoint), 91020100 = the section area itself;
 *                   the next area change to an area of the same kind (0x92.. / 0x91..) loads the wanted one instead (one shot)
 *   warp off | status
 *
 * Two layers. (a) RECORD level (ids 0x91EESS00, the working one): the engine looks the section's episode record up by id with 0x726B70 (thiscall, ecx = the episode
 * table, id on the stack, ret 4; the Continue path gets the id from the save slot); a 6-byte detour (56 8B 71 6C 33 D2 = push esi; mov esi,[ecx+6c]; xor edx,edx) swaps the
 * id of every story-section lookup for the wanted one until `warp off`, so the save work, the area class, the stage and the start position all belong to one section.
 * (b) AREA level (ids 0x92EESS00, only the area class changes: the units are the wanted section's but the STAGE stays the old one; kept for the log). Mechanism: every area change goes through sGameArea (vtable 0xFF25AC) slot 10 = 0x43C740, thiscall(this, DTI of the next area) ret 4. The area class of a
 * story section is aEpisodeEE.. whose DTI hash IS the scene id 0x91EESS00 (0x91FF0000 = "no next section", back to the title). The wrapper swaps the DTI for the one of the
 * wanted id (found in the engine's DTI hash table, 0x1185C60 + (id & 0xFF) * 4, chain link +0x14, hash +0x1C) the first time a story section is requested; every
 * area change is logged. Earlier tries that did NOT work: the title menu manager's update (the record is set and consumed inside one update) and the loader
 * 0x6D0FF0 / aStartContinue's update (neither runs on the Campaign -> Continue path).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "r1.h"

#define AREA_VTABLE 0xFF25ACu
#define AREA_SLOT 10
#define AREA_ORIG 0x43C740u
#define DTI_TABLE 0x1185C60u
#define DTI_NEXT 0x14u
#define DTI_HASH 0x1Cu

typedef void(__attribute__((fastcall)) * change_t)(void *self, void *edx, void *dti);
static change_t orig_change;
static volatile unsigned g_id;       /* 0 = off */
static unsigned g_calls, g_swaps, g_last_from;
static int g_installed;

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    return r1_safe_read((void *)(uintptr_t)a, &v, 4) ? v : 0;
}

static unsigned dti_by_hash(unsigned id)
{
    unsigned node = rd32(DTI_TABLE + 4 * (id & 0xFF));
    for (int n = 0; node && n < 2048; n++, node = rd32(node + DTI_NEXT))
        if (rd32(node + DTI_HASH) == id)
            return node;
    return 0;
}

static void __attribute__((fastcall)) hook_change(void *self, void *edx, void *dti)
{
    g_calls++;
    unsigned hash = dti ? rd32((unsigned)(uintptr_t)dti + DTI_HASH) : 0;
    r1_log("warp: area change requested: DTI %08x hash %08x%s", (unsigned)(uintptr_t)dti, hash, g_id ? " (warp armed)" : "");
    if (g_id && (hash >> 24) == (g_id >> 24) && (hash & 0x00FF0000u) != 0x00FF0000u && hash != g_id) {
        unsigned want = dti_by_hash(g_id);
        if (want) {
            r1_log("warp: story section %08x replaced by %08x (DTI %08x)", hash, g_id, want);
            g_last_from = hash;
            g_swaps++;
            g_id = 0;
            dti = (void *)(uintptr_t)want;
        } else
            r1_log("warp: no DTI with hash %08x in the engine's table: the request goes through unchanged", g_id);
    }
    orig_change(self, edx, dti);
}

/* ---- (a) the episode record lookup 0x726B70 ---- */
#define LOOKUP_VA 0x726B70u
#define LOOKUP_STEAL 6
typedef void *(__attribute__((fastcall)) * lookup_t)(void *table, void *edx, unsigned id);
static lookup_t g_lookup_tramp;
static unsigned g_lookup_calls, g_lookup_swaps, g_lookup_last_from;

static void *__attribute__((fastcall)) hook_lookup(void *table, void *edx, unsigned id)
{
    g_lookup_calls++;
    if (g_id && (g_id >> 24) == 0x91u && (id >> 24) == 0x91u && (id & 0x00FF0000u) != 0x00FF0000u && id != g_id) {
        g_lookup_swaps++;
        g_lookup_last_from = id;
        if (g_lookup_swaps <= 20)
            r1_log("warp: episode record lookup %08x replaced by %08x", id, g_id);
        id = g_id;
    }
    return g_lookup_tramp(table, edx, id);
}

static int install_lookup_detour(void)
{
    static const unsigned char expect[LOOKUP_STEAL] = {0x56, 0x8B, 0x71, 0x6C, 0x33, 0xD2};
    unsigned char cur[LOOKUP_STEAL];
    if (!r1_safe_read((void *)(uintptr_t)LOOKUP_VA, cur, LOOKUP_STEAL) || memcmp(cur, expect, LOOKUP_STEAL)) {
        r1_log("warp: the lookup's first bytes are not the expected ones: the record layer stays off");
        return 0;
    }
    unsigned char *t = (unsigned char *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t)
        return 0;
    memcpy(t, expect, LOOKUP_STEAL);
    t[LOOKUP_STEAL] = 0xE9;
    *(int *)(t + LOOKUP_STEAL + 1) = (int)((LOOKUP_VA + LOOKUP_STEAL) - (unsigned)(uintptr_t)(t + LOOKUP_STEAL + 5));
    g_lookup_tramp = (lookup_t)(uintptr_t)t;
    unsigned char patch[LOOKUP_STEAL];
    patch[0] = 0xE9;
    *(int *)(patch + 1) = (int)((unsigned)(uintptr_t)hook_lookup - (LOOKUP_VA + 5));
    patch[5] = 0x90;
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)LOOKUP_VA, LOOKUP_STEAL, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(uintptr_t)LOOKUP_VA, patch, LOOKUP_STEAL);   /* one copy of whole instructions; the lookup runs only in menu / section changes (we are idle at the title) */
    VirtualProtect((void *)(uintptr_t)LOOKUP_VA, LOOKUP_STEAL, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)LOOKUP_VA, LOOKUP_STEAL);
    r1_log("warp: episode record lookup %08x detoured (trampoline %p)", LOOKUP_VA, (void *)t);
    return 1;
}

static int install_hook(void)
{
    unsigned cur = rd32(AREA_VTABLE + 4 * AREA_SLOT);
    if (cur != AREA_ORIG) {
        r1_log("warp: sGameArea vtable slot %d is %08x, expected %08x: the tool stays off", AREA_SLOT, cur, AREA_ORIG);
        return 0;
    }
    void *fake = (void *)(uintptr_t)AREA_VTABLE;
    orig_change = (change_t)(uintptr_t)cur;
    if (!r1_patch_vtable(&fake, AREA_SLOT, (void *)hook_change)) {
        r1_log("warp: cannot patch the vtable slot");
        return 0;
    }
    r1_log("warp: sGameArea slot %d wrapped", AREA_SLOT);
    return 1;
}

void warp_install(void)
{
    /* nothing at start: the wrapper is put in by the first `warp <id>` */
}

void warp_cmd(const char *arg, char *out, int size)
{
    if (!arg || !arg[0] || !strcmp(arg, "status")) {
        snprintf(out, size, "warp: area wrapper %s, record lookup detour %s, id armed %08x; area changes seen %u, swaps %u (last %08x); lookups %u, swaps %u (last %08x)",
                 g_installed ? "in place" : "not installed", g_lookup_tramp ? "in place" : "not installed", g_id, g_calls, g_swaps, g_last_from,
                 g_lookup_calls, g_lookup_swaps, g_lookup_last_from);
        return;
    }
    if (!strcmp(arg, "off")) {
        g_id = 0;
        snprintf(out, size, "warp: off");
        return;
    }
    unsigned id = (unsigned)strtoul(arg, NULL, 16);
    if ((id >> 24) != 0x91u && (id >> 24) != 0x92u) {
        snprintf(out, size, "warp: a scene id looks like 91EESS00 or 92EESS00 (hex), got '%s'", arg);
        return;
    }
    if (!dti_by_hash(id)) {
        snprintf(out, size, "warp: no area class with hash %08x in the engine's DTI table", id);
        return;
    }
    if (!g_lookup_tramp && (id >> 24) == 0x91u)
        install_lookup_detour();
    if (!g_installed && !(g_installed = install_hook())) {
        snprintf(out, size, "warp: cannot install the wrapper (see the log)");
        return;
    }
    g_id = id;
    snprintf(out, size, "warp: the next story section request loads section %08x", id);
}
