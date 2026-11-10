/*
 * guards1_game.c - the glue of the network guards (guards1.c) to the running game: process memory, the REV1_GUARDS variable, the network session of
 * net1.c, the own slot of capture.c, one pass per logic frame and the console command `guards`.
 *
 * Off by default: a single-player game and the split screen run on the original bytes. The patches are written when a network session has
 * STARTED (net1_session_active) or REV1_GUARDS=1, or by the console (`guards apply`), and taken out again when the session ends (not for
 * REV1_GUARDS=1 / `guards apply`, which stay until `guards revert` / `guards auto`). A refusal (the bytes are not those of build 13298487) is
 * not retried every frame: it is retried when the wish changes from off to on.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"
#include "capture.h"
#include "net1.h"
#include "guards1.h"

static g1_t G;
static int inited, last_want;
static CRITICAL_SECTION cs;

static int m_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    return r1_safe_read((const void *)(uintptr_t)a, out, n);
}

static int m_write(void *u, uint32_t a, const void *p, uint32_t n)
{
    (void)u;
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(uintptr_t)a, p, n);
    VirtualProtect((void *)(uintptr_t)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)a, n);
    return 1;
}

static uint32_t m_alloc(void *u, uint32_t n)
{
    (void)u;
    return (uint32_t)(uintptr_t)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);   /* the process is 32-bit: the address fits */
}

static void m_log(void *u, const char *line)
{
    (void)u;
    r1_log("%s", line);
}

static const g1_mem MEM = {m_read, m_write, m_alloc, m_log, NULL};

static uint32_t own_slot(void)
{
    int o = capture_own_effective();
    return o > 0 ? 1u : 0u;
}

static void apply_all(void)
{
    uint32_t own = own_slot();
    g1_apply(&G, &MEM, G1_EXIT, own);
    g1_apply(&G, &MEM, G1_PICK, own);
}

static void revert_all(void)
{
    g1_revert(&G, &MEM, G1_EXIT);
    g1_revert(&G, &MEM, G1_PICK);
}

static void guards_frame(void)
{
    EnterCriticalSection(&cs);
    int want = g1_want(&G, net1_session_active());
    if (want && !last_want)
        apply_all();                                  /* off -> on: write, or refuse and say why */
    else if (want && (G.state[G1_EXIT] == G1_ON || G.state[G1_PICK] == G1_ON))
        g1_set_own(&G, &MEM, own_slot());             /* the cell follows `cap own` / the role of the session */
    else if (!want && last_want)
        revert_all();
    last_want = want;
    LeaveCriticalSection(&cs);
}

int guards_init(void)
{
    if (inited)
        return 0;
    inited = 1;
    InitializeCriticalSection(&cs);
    char v[16];
    g1_init(&G, r1_env("REV1_GUARDS", v, sizeof v) && atoi(v) == 1);
    lt_add_frame_cb(guards_frame);
    r1_log("guards: module ready (REV1_GUARDS=%d; the exe+1396 and pickup patches are written only in a network session, with REV1_GUARDS=1 or by `guards apply`)", G.env_on);
    return 0;
}

void guards_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "";
    sscanf(arg, "%15s", sub);
    EnterCriticalSection(&cs);
    if (!inited) {
        snprintf(out, size, "guards: not initialised");
    } else if (!strcmp(sub, "apply")) {
        G.mode = G1_MODE_ON;
        apply_all();
        last_want = 1;
        g1_status(&G, &MEM, net1_session_active(), own_slot(), out, (size_t)size);
    } else if (!strcmp(sub, "revert")) {
        G.mode = G1_MODE_OFF;
        revert_all();
        last_want = 0;
        g1_status(&G, &MEM, net1_session_active(), own_slot(), out, (size_t)size);
    } else if (!strcmp(sub, "auto")) {
        G.mode = G1_MODE_AUTO;   /* the next frame decides: session / REV1_GUARDS */
        g1_status(&G, &MEM, net1_session_active(), own_slot(), out, (size_t)size);
    } else if (!sub[0] || !strcmp(sub, "status")) {
        g1_status(&G, &MEM, net1_session_active(), own_slot(), out, (size_t)size);
    } else {
        snprintf(out, size, "guards status | apply (force on) | revert (force off) | auto (session / REV1_GUARDS decides); the pickup follows `cap own <slot>`");
    }
    LeaveCriticalSection(&cs);
}
