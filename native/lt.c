/*
 * Run code on the game's logic thread, and keep a background test window "active" for the game.
 *
 * The game's logic thread (start routine 0x9BA3E0, created by sApp's init at 0x9BC989) loops
 *     if (app[0x20] == 0 && app[0x2551] == 0) { Sleep(1); continue; }     // window not active: idle
 *     ... renderer->0xB6D4F0()  (render step)   then   sApp->vf[6]()  (frame update, runs the game areas)
 * At 0x9BA48D the loop loads `mov ecx,[renderer]` (8B 0D 9C 1B 1E 01, eax/ecx/edx dead there). We replace those 6 bytes by `call stub; nop`; the stub
 * calls lt_hook() and then does the original `mov ecx,[renderer]; ret`. lt_hook therefore runs once per pass ON THE GAME THREAD, between two
 * updates: tasks posted by the console thread run there (`L <cmd>` or any command that touches game state), plus per-pass callbacks.
 * Same mechanism as Revelations 2 (coop3.c), Rev1 addresses.
 *
 * The "active" byte: WM_ACTIVATEAPP(true)/WM_NCACTIVATE(true) set app[0x2551] = 1; a WM_ACTIVATE that arrives while app[0x2657] == 0 clears it
 * (SetActive(false) at 0x9BA110) and the thread idles in Sleep(1). In background test mode (REV1_BACKGROUND=1) the window is never really
 * activated, so a watchdog thread keeps the byte at 1 (stage 2a of Revelations 2: the start was erratic without it). Only in that mode.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((void *)a, &v, 4);
    return v;
}

static volatile LONG lt_ready;
static volatile DWORD lt_tid;
static volatile unsigned lt_pass;
/* (world-downed, wp4 live 09:15: with downed1 the 9th module, world_frame was the 10th and silently dropped by the old limit of 8 - the world hooks never came on) */
#define LT_MAX_CBS 24
static lt_frame_cb cbs[LT_MAX_CBS];
static int n_cbs;

static struct {
    volatile LONG state;   /* 0 idle, 1 posted, 2 running, 3 done */
    char cmd[400];
    char out[7000];
    int outsize;
} task;
static CRITICAL_SECTION task_lock;
static HANDLE task_done;

unsigned lt_frames(void) { return lt_pass; }
void lt_add_frame_cb(lt_frame_cb cb)
{
    if (n_cbs < LT_MAX_CBS)
        cbs[n_cbs++] = cb;
    else
        r1_log("lt: FRAME CALLBACK TABLE FULL (%d): a module's per-pass work is NOT registered", LT_MAX_CBS);
}

static void lt_hook(void)
{
    lt_tid = GetCurrentThreadId();
    lt_pass++;
    if (task.state == 1 && InterlockedCompareExchange(&task.state, 2, 1) == 1) {
        task.out[0] = 0;
        probe_run(task.cmd, task.out, task.outsize);
        InterlockedExchange(&task.state, 3);
        SetEvent(task_done);
    }
    for (int i = 0; i < n_cbs; i++)
        cbs[i]();
}

static DWORD WINAPI keepalive_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(50);
        unsigned app = rd32(R1_APP_PTR);
        if (app && !(rd32(app + 0x2660) & 0xFF)) {
            unsigned char one = 1, cur = 0;
            r1_safe_read((void *)(app + 0x2551), &cur, 1);
            if (!cur)
                WriteProcessMemory(GetCurrentProcess(), (void *)(app + 0x2551), &one, 1, NULL);
        }
    }
    return 0;
}

int lt_on_game_thread(void) { return lt_ready && GetCurrentThreadId() == lt_tid; }

/* Run cmd on the game thread; the answer is copied to out. */
void lt_post(const char *cmd, char *out, int size)
{
    if (!lt_ready) {
        snprintf(out, size, "logic hook not installed (game not rendering yet?)");
        return;
    }
    if (lt_on_game_thread()) {
        probe_run(cmd, out, size);
        return;
    }
    EnterCriticalSection(&task_lock);
    snprintf(task.cmd, sizeof task.cmd, "%s", cmd);
    task.outsize = sizeof task.out;
    ResetEvent(task_done);
    InterlockedExchange(&task.state, 1);
    DWORD w = WaitForSingleObject(task_done, 6000);
    if (w != WAIT_OBJECT_0) {
        if (InterlockedCompareExchange(&task.state, 0, 1) == 1) {
            snprintf(out, size, "timeout: the game loop did not run the task in 6 s (paused/idle?); cancelled");
            LeaveCriticalSection(&task_lock);
            return;
        }
        WaitForSingleObject(task_done, 20000);   /* it is running: wait for it */
    }
    snprintf(out, size, "%s", task.out);
    InterlockedExchange(&task.state, 0);
    LeaveCriticalSection(&task_lock);
}

void lt_install(void)
{
    static int done;
    if (done || !r1_text_decrypted())
        return;
    done = 1;
    static const unsigned char expect[] = {0x8B, 0x0D, 0x9C, 0x1B, 0x1E, 0x01, 0xE8};
    unsigned char cur[7];
    if (!r1_safe_read((void *)R1_LOOP_HOOK, cur, 7) || memcmp(cur, expect, 7) != 0) {
        r1_log("lt: loop bytes at %08x differ from the supported build (%02x %02x %02x %02x %02x %02x %02x), logic hook NOT installed", R1_LOOP_HOOK, cur[0],
               cur[1], cur[2], cur[3], cur[4], cur[5], cur[6]);
        return;
    }
    InitializeCriticalSection(&task_lock);
    task_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    unsigned char *stub = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub)
        return;
    stub[0] = 0xE8;
    *(int *)(stub + 1) = (int)((unsigned)lt_hook - (unsigned)(stub + 5));
    static const unsigned char tail[] = {0x8B, 0x0D, 0x9C, 0x1B, 0x1E, 0x01, 0xC3};
    memcpy(stub + 5, tail, sizeof tail);
    DWORD old;
    if (!VirtualProtect((void *)R1_LOOP_HOOK, 6, PAGE_EXECUTE_READWRITE, &old))
        return;
    unsigned char *p = (unsigned char *)R1_LOOP_HOOK;
    p[0] = 0xE8;
    *(int *)(p + 1) = (int)((unsigned)stub - (R1_LOOP_HOOK + 5));
    p[5] = 0x90;
    VirtualProtect((void *)R1_LOOP_HOOK, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)R1_LOOP_HOOK, 6);
    InterlockedExchange(&lt_ready, 1);
    if (testmode_background())
        CreateThread(NULL, 0, keepalive_thread, NULL, 0, NULL);
    r1_log("lt: logic-thread hook installed at %08x (stub %p), thread %lu", R1_LOOP_HOOK, stub, GetCurrentThreadId());
}
