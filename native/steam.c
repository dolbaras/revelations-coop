/*
 * The game's own Steam calls (import table of steam_api.dll plus the ISteamUserStats vtable behind SteamInternal_ContextInit). LOGGING ONLY:
 * every hook calls the original with the original arguments and returns its result unchanged; nothing is faked, nothing is stored by the mod.
 *
 *   - SteamAPI_Init / SteamAPI_RunCallbacks timing (console "steam")
 *   - the context (CSteamAPIContext, field +0x14 = ISteamUserStats*): its vtable slots for RequestCurrentStats, GetStat/SetStat, GetAchievement,
 *     SetAchievement, ClearAchievement, GetAchievementAndUnlockTime, StoreStats, IndicateAchievementProgress, RequestUserStats, ResetAllStats
 *     are wrapped and every call is written to the mod's log as "ach: ..." (names and values). Stage 2 live check of achievements-map.md section 1:
 *     the exact Steam API names the game passes (snprintf(buf, "%d", id) expected) and whether SetAchievement is the only write.
 *
 * Slot numbers are those of Valve's interface headers (ISteamUserStats = RequestCurrentStats 0, GetStat 1-2, SetStat 3-4, UpdateAvgRateStat 5,
 * GetAchievement 6, SetAchievement 7, ClearAchievement 8, GetAchievementAndUnlockTime 9, StoreStats 10, ..., IndicateAchievementProgress 13,
 * RequestUserStats 16, ResetAllStats 21); the static analysis of the exe used +0x18 (GetAchievement) and +0x1C (SetAchievement) = slots 6 and 7.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "r1.h"

#define THISCALL __attribute__((thiscall))

typedef int (__cdecl *SteamAPI_Init_t)(void);
typedef void (__cdecl *SteamAPI_RunCallbacks_t)(void);
typedef void *(__cdecl *ContextInit_t)(void *data);
static SteamAPI_Init_t real_Init;
static SteamAPI_RunCallbacks_t real_RunCallbacks;
static ContextInit_t real_ContextInit;
static volatile LONG rc_calls, rc_slow, rc_max_ms;
static volatile LONG rc_total_ms;
static DWORD t_start;
static int log_stats;
static int ach_block;   /* REV1_ACHBLOCK=1 (test runs): SetAchievement / SetStat / StoreStats are logged and NOT passed to Steam: no real achievement or stat is ever written by a test */

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

static const char *who(const void *addr, char *buf, int size)
{
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, 92);
        snprintf(buf, size, "%s+%lx", b ? b + 1 : name, (unsigned long)((const char *)addr - (const char *)m));
    } else
        snprintf(buf, size, "%p", addr);
    return buf;
}

static int __cdecl hook_Init(void)
{
    double t = now_ms();
    int r = real_Init();
    r1_log("steam: SteamAPI_Init -> %d in %.0f ms", r, now_ms() - t);
    return r;
}

static void __cdecl hook_RunCallbacks(void)
{
    double t = now_ms();
    real_RunCallbacks();
    LONG ms = (LONG)(now_ms() - t);
    InterlockedIncrement(&rc_calls);
    InterlockedAdd(&rc_total_ms, ms);
    if (ms > rc_max_ms)
        rc_max_ms = ms;
    if (ms >= 100 && InterlockedIncrement(&rc_slow) <= 30)
        r1_log("steam: SteamAPI_RunCallbacks took %ld ms (call %ld)", ms, rc_calls);
}

void steam_stats(char *out, int size)
{
    snprintf(out, size, "steam: %ld RunCallbacks calls, %ld slow (>=100 ms), longest %ld ms, %ld ms in total", rc_calls, rc_slow, rc_max_ms, rc_total_ms);
}

static int caller_is_exe(const void *ret)
{
    HMODULE exe = GetModuleHandleA(NULL);
    return ret >= (const void *)exe && ret < (const void *)((char *)exe + 0x1300000);
}

static void *stat_orig[32];
static void **stats_vtbl;

#define CALLER_TAG(r) (caller_is_exe(r) ? "exe" : "other")

static char caller_buf[8][80];
static const char *caller_str(const void *ret, int k)
{
    return who(ret, caller_buf[k & 7], sizeof caller_buf[0]);
}

static int THISCALL s_RequestCurrentStats(void *self)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *))stat_orig[0])(self);
    r1_log("ach: RequestCurrentStats() -> %d (caller %s, +%lu ms)", r & 0xff, caller_str(ret, 0), GetTickCount() - t_start);
    return r;
}

#define GETSTAT(N) \
    static int THISCALL s_GetStat##N(void *self, const char *name, void *out) \
    { \
        const void *ret = __builtin_return_address(0); \
        int r = ((int (THISCALL *)(void *, const char *, void *))stat_orig[N])(self, name, out); \
        unsigned v = 0; \
        r1_safe_read(out, &v, 4); \
        float f; memcpy(&f, &v, 4); \
        r1_log("ach: GetStat[%d](\"%s\") -> %d value int=%d float=%g (caller %s)", N, name ? name : "(null)", r & 0xff, (int)v, (double)f, caller_str(ret, N)); \
        return r; \
    }
GETSTAT(1)
GETSTAT(2)

#define SETSTAT(N) \
    static int THISCALL s_SetStat##N(void *self, const char *name, unsigned v) \
    { \
        const void *ret = __builtin_return_address(0); \
        float f; memcpy(&f, &v, 4); \
        if (ach_block) { r1_log("ach: SetStat[%d] BLOCKED (REV1_ACHBLOCK)", N); return 1; } \
        int r = ((int (THISCALL *)(void *, const char *, unsigned))stat_orig[N])(self, name, v); \
        r1_log("ach: SetStat[%d](\"%s\", int=%d float=%g) -> %d (caller %s, +%lu ms)", N, name ? name : "(null)", (int)v, (double)f, r & 0xff, caller_str(ret, N), GetTickCount() - t_start); \
        return r; \
    }
SETSTAT(3)
SETSTAT(4)

static int THISCALL s_UpdateAvgRateStat(void *self, const char *name, float count, double length)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, const char *, float, double))stat_orig[5])(self, name, count, length);
    r1_log("ach: UpdateAvgRateStat(\"%s\", %g, %g) -> %d (caller %s)", name ? name : "(null)", (double)count, length, r & 0xff, caller_str(ret, 5));
    return r;
}

static int THISCALL s_GetAchievement(void *self, const char *name, unsigned char *got)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, const char *, unsigned char *))stat_orig[6])(self, name, got);
    unsigned char g = 0xff;
    r1_safe_read(got, &g, 1);
    r1_log("ach: GetAchievement(\"%s\") -> %d achieved=%d (caller %s)", name ? name : "(null)", r & 0xff, g, caller_str(ret, 6));
    return r;
}

static int THISCALL s_SetAchievement(void *self, const char *name)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) {
        r1_log("ach: SetAchievement(\"%s\") BLOCKED (REV1_ACHBLOCK), not passed to Steam (caller %s)", name ? name : "(null)", caller_str(ret, 7));
        return 1;
    }
    int r = ((int (THISCALL *)(void *, const char *))stat_orig[7])(self, name);
    r1_log("ach: SetAchievement(\"%s\") -> %d (caller %s, +%lu ms)", name ? name : "(null)", r & 0xff, caller_str(ret, 7), GetTickCount() - t_start);
    return r;
}

static int THISCALL s_ClearAchievement(void *self, const char *name)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, const char *))stat_orig[8])(self, name);
    r1_log("ach: ClearAchievement(\"%s\") -> %d (caller %s)", name ? name : "(null)", r & 0xff, caller_str(ret, 8));
    return r;
}

static int THISCALL s_GetAchievementAndUnlockTime(void *self, const char *name, unsigned char *got, unsigned *when)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, const char *, unsigned char *, unsigned *))stat_orig[9])(self, name, got, when);
    unsigned char g = 0xff;
    unsigned w = 0;
    r1_safe_read(got, &g, 1);
    r1_safe_read(when, &w, 4);
    r1_log("ach: GetAchievementAndUnlockTime(\"%s\") -> %d achieved=%d time=%u (caller %s)", name ? name : "(null)", r & 0xff, g, w, caller_str(ret, 9));
    return r;
}

static int THISCALL s_StoreStats(void *self)
{
    const void *ret = __builtin_return_address(0);
    if (ach_block) {
        r1_log("ach: StoreStats() BLOCKED (REV1_ACHBLOCK) (caller %s)", caller_str(ret, 10));
        return 1;
    }
    int r = ((int (THISCALL *)(void *))stat_orig[10])(self);
    r1_log("ach: StoreStats() -> %d (caller %s, +%lu ms)", r & 0xff, caller_str(ret, 10), GetTickCount() - t_start);
    return r;
}

static int THISCALL s_IndicateAchievementProgress(void *self, const char *name, unsigned cur, unsigned max)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, const char *, unsigned, unsigned))stat_orig[13])(self, name, cur, max);
    r1_log("ach: IndicateAchievementProgress(\"%s\", %u, %u) -> %d (caller %s)", name ? name : "(null)", cur, max, r & 0xff, caller_str(ret, 13));
    return r;
}

static unsigned long long THISCALL s_RequestUserStats(void *self, unsigned lo, unsigned hi)
{
    const void *ret = __builtin_return_address(0);
    unsigned long long r = ((unsigned long long (THISCALL *)(void *, unsigned, unsigned))stat_orig[16])(self, lo, hi);
    r1_log("ach: RequestUserStats(steamid %08x%08x) -> call %llx (caller %s)", hi, lo, r, caller_str(ret, 16));
    return r;
}

static int THISCALL s_ResetAllStats(void *self, int ach)
{
    const void *ret = __builtin_return_address(0);
    int r = ((int (THISCALL *)(void *, int))stat_orig[21])(self, ach);
    r1_log("ach: ResetAllStats(achievements too=%d) -> %d (caller %s)", ach & 0xff, r & 0xff, caller_str(ret, 21));
    return r;
}

static void wrap_stats(void *iface, const char *ver)
{
    void **vt = *(void ***)iface;
    if (vt == stats_vtbl)
        return;
    stats_vtbl = vt;
    static const struct { int slot; void *fn; } map[] = {
        {0, (void *)s_RequestCurrentStats}, {1, (void *)s_GetStat1}, {2, (void *)s_GetStat2}, {3, (void *)s_SetStat3}, {4, (void *)s_SetStat4},
        {5, (void *)s_UpdateAvgRateStat}, {6, (void *)s_GetAchievement}, {7, (void *)s_SetAchievement}, {8, (void *)s_ClearAchievement},
        {9, (void *)s_GetAchievementAndUnlockTime}, {10, (void *)s_StoreStats}, {13, (void *)s_IndicateAchievementProgress},
        {16, (void *)s_RequestUserStats}, {21, (void *)s_ResetAllStats}};
    r1_log("ach: ISteamUserStats %s at %p, vtable %p: wrapping %d slots (logging only)", ver, iface, (void *)vt, (int)(sizeof map / sizeof map[0]));
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        DWORD old;
        if (!VirtualProtect(&vt[map[i].slot], sizeof(void *), PAGE_READWRITE, &old))
            continue;
        stat_orig[map[i].slot] = vt[map[i].slot];
        vt[map[i].slot] = map[i].fn;
        VirtualProtect(&vt[map[i].slot], sizeof(void *), old, &old);
    }
}

static void *__cdecl hook_ContextInit(void *data)
{
    void *r = real_ContextInit(data);
    static int n;
    if (r) {
        void *stats = NULL;
        r1_safe_read((char *)r + 0x14, &stats, 4);
        if (++n <= 3) {
            unsigned d[8];
            r1_safe_read(r, d, sizeof d);
            r1_log("steam: SteamInternal_ContextInit(%p) -> %p [%08x %08x %08x %08x %08x %08x %08x %08x]", data, r, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }
        if (stats && log_stats)
            wrap_stats(stats, "STEAMUSERSTATS (context +0x14)");
    }
    return r;
}

void steam_install(void)
{
    char v[8];
    t_start = GetTickCount();
    log_stats = !(r1_env("REV1_NOACH", v, sizeof v) && v[0] == '1');
    ach_block = r1_env("REV1_ACHBLOCK", v, sizeof v) && v[0] == '1';
    if (ach_block)
        log_stats = 1;
    real_Init = (SteamAPI_Init_t)r1_patch_iat("steam_api.dll", "SteamAPI_Init", (void *)hook_Init);
    real_RunCallbacks = (SteamAPI_RunCallbacks_t)r1_patch_iat("steam_api.dll", "SteamAPI_RunCallbacks", (void *)hook_RunCallbacks);
    real_ContextInit = (ContextInit_t)r1_patch_iat("steam_api.dll", "SteamInternal_ContextInit", (void *)hook_ContextInit);
}
