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
#include "coop_steam.h"
#include "net1.h"

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
    cst_game_tick();   /* round 2, the Steam way (port of Rev2's steam.c): the co-op transport on the game thread registers its callbacks, pumps its events, starts an invited join */
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

static void *g_ctx;
void *steam_context(void) { return g_ctx; }

/* lobby-must: the name the network partner sees = the player's Steam persona name (public on Steam anyway), never the computer or the Windows user name.
 * CSteamAPIContext [2] = ISteamFriends (SDK 1.2x/1.3x: client, user, friends, ...; the same slots coop_steam_real.c uses), vtable slot 0 = GetPersonaName (thiscall,
 * every interface version). Read once per call into a static buffer; NULL before the context or when the pointers look wrong. */
const char *steam_persona_name(void)
{
    static char name[64];
    void *fr = NULL, **vt = NULL, *fn = NULL;
    if (!g_ctx || !r1_safe_read((char *)g_ctx + 8, &fr, 4) || !fr || !r1_safe_read(fr, &vt, 4) || !vt || !r1_safe_read(vt, &fn, 4) || !fn) return NULL;
    const char *p = ((const char *(THISCALL *)(void *))fn)(fr);
    if (!p) return NULL;
    char tmp[64];
    memset(tmp, 0, sizeof tmp);
    if (!r1_safe_read(p, tmp, 1)) return NULL;
    for (int i = 0; i < (int)sizeof tmp - 1; i++)
        if (!r1_safe_read(p + i, &tmp[i], 1) || !tmp[i]) break;
    tmp[sizeof tmp - 1] = 0;
    if (!tmp[0]) return NULL;
    memcpy(name, tmp, sizeof name);
    return name;
}

static void *__cdecl hook_ContextInit(void *data)
{
    void *r = real_ContextInit(data);
    static int n;
    if (r) {
        g_ctx = r;
        cst_note_context(r);   /* the co-op transport uses the interfaces the game itself holds (coop_steam_real.c; same versions as Rev2: Client017, User019, Friends015, Utils009, MatchMaking009, Networking005) */
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

/* ---- round 2, the Steam way: the callbacks the game registers (port of Rev2's steam.c). Every callback object of the game gets its two Run slots wrapped so that a
 * callback about OUR lobby (333 Join game, 505 / 506 lobby enter / data: cst_swallow_game_callback, the rule = our tag or no Raid key; Rev1's Raid lobby keys are
 * Rev2's: Name, OwnerId, SlotPublicMax/Open, SlotPrivateMax/Open, SearchKeyNum, SearchKey%d, BinarySize, BinaryData at 0x10C0574..) never reaches the game's Raid
 * handlers; everything else runs the game's own code unchanged. REV2_STEAM_SWALLOW=0 (the name coop_steam.c kept) = never swallow. */
typedef void (__cdecl *RegisterCallback_t)(void *cb, int id);
typedef void (__cdecl *UnregisterCallback_t)(void *cb);
typedef void (__cdecl *RegisterCallResult_t)(void *cb, unsigned long long call);
typedef void (__cdecl *UnregisterCallResult_t)(void *cb, unsigned long long call);
static RegisterCallback_t real_RegisterCallback;
static UnregisterCallback_t real_UnregisterCallback;
static RegisterCallResult_t real_RegisterCallResult;
static UnregisterCallResult_t real_UnregisterCallResult;
typedef void (THISCALL *Run1_t)(void *self, void *pv);
typedef void (THISCALL *Run3_t)(void *self, void *pv, int fail, unsigned long long call);
static struct { void **vtbl; void *orig[2]; int kind[2]; int count; int size; } cbv[160];   /* kind: 1 = Run(pv), 3 = Run(pv, fail, call) */
static int n_cbv;
static volatile LONG n_runs;

/* the slot's kind from its return: ret 4 = Run(pv), ret 0x10 = Run(pv, fail, call) */
static int ret_kind(const unsigned char *fn)
{
    unsigned char c[96];
    if (!r1_safe_read(fn, c, sizeof c)) return 0;
    for (int i = 0; i + 2 < (int)sizeof c; i++)
        if (c[i] == 0xC2 && c[i + 2] == 0) {
            if (c[i + 1] == 0x04) return 1;
            if (c[i + 1] == 0x10) return 3;
        }
    return 0;
}

static int cb_find(void *self)
{
    void **vt = *(void ***)self;
    for (int i = 0; i < n_cbv; i++)
        if (cbv[i].vtbl == vt) return i;
    return -1;
}

static void THISCALL hook_run1(void *self, void *pv)
{
    int i = cb_find(self);
    if (i < 0) return;
    int id = *(int *)((char *)self + 8);
    int slot = cbv[i].kind[0] == 1 ? 0 : 1;
    cbv[i].count++;
    if (InterlockedIncrement(&n_runs) <= 60) r1_log("steam: callback %d runs (Run(param), +%lu ms)", id, GetTickCount() - t_start);
    if (cst_swallow_game_callback(id, pv)) {
        r1_log("steam: callback %d about our lobby kept from the game's handler", id);
        return;
    }
    ((Run1_t)cbv[i].orig[slot])(self, pv);
}

static void THISCALL hook_run3(void *self, void *pv, int fail, unsigned long long call)
{
    int i = cb_find(self);
    if (i < 0) return;
    int id = *(int *)((char *)self + 8);
    int slot = cbv[i].kind[0] == 3 ? 0 : 1;
    cbv[i].count++;
    if (InterlockedIncrement(&n_runs) <= 60) r1_log("steam: callback %d runs (Run(param, failure=%d, call=%llx), +%lu ms)", id, fail, call, GetTickCount() - t_start);
    if (!call && cst_swallow_game_callback(id, pv)) {
        r1_log("steam: callback %d about our lobby kept from the game's handler", id);
        return;
    }
    ((Run3_t)cbv[i].orig[slot])(self, pv, fail, call);
}

static void wrap_callback(void *cb, int size)
{
    if (!cb || cb_find(cb) >= 0 || n_cbv >= 160) return;
    void **vt = *(void ***)cb;
    int k0 = ret_kind((unsigned char *)vt[0]), k1 = ret_kind((unsigned char *)vt[1]);
    if (k0 && !k1) k1 = k0 == 1 ? 3 : 1;   /* a callback object has exactly Run(param) and Run(param, failure, call) in slots 0 and 1 */
    else if (k1 && !k0) k0 = k1 == 1 ? 3 : 1;
    int i = n_cbv++;
    cbv[i].vtbl = vt;
    cbv[i].orig[0] = vt[0], cbv[i].orig[1] = vt[1];
    cbv[i].kind[0] = k0, cbv[i].kind[1] = k1, cbv[i].size = size;
    char b[80];
    r1_log("steam: handler vtable %s Run slots: [0]=%s [1]=%s", who(vt, b, sizeof b), k0 == 1 ? "Run(param)" : k0 == 3 ? "Run(param,failure,call)" : "?",
           k1 == 1 ? "Run(param)" : k1 == 3 ? "Run(param,failure,call)" : "?");
    if (!((k0 == 1 && k1 == 3) || (k0 == 3 && k1 == 1))) return;
    cst_note_callback_layout(k0);   /* our own callback objects (coop_steam_real.c) use the same slot order */
    DWORD old;
    if (!VirtualProtect(&vt[0], 2 * sizeof(void *), PAGE_READWRITE, &old)) return;
    vt[0] = k0 == 1 ? (void *)hook_run1 : (void *)hook_run3;
    vt[1] = k1 == 1 ? (void *)hook_run1 : (void *)hook_run3;
    VirtualProtect(&vt[0], 2 * sizeof(void *), old, &old);
}

static int cb_size(void **vt)
{
    unsigned char sz[8];
    return vt && r1_safe_read(vt[2], sz, 6) && sz[0] == 0xB8 ? *(int *)(sz + 1) : -1;   /* slot 2 = GetCallbackSizeBytes: mov eax, imm32 */
}

static void __cdecl hook_RegisterCallback(void *cb, int id)
{
    char b[80], c[80];
    void **vt = cb ? *(void ***)cb : NULL;
    int size = cb_size(vt);
    r1_log("steam: RegisterCallback id=%d (callback struct %d bytes) object %p vtable %s caller %s (+%lu ms)", id, size, cb, vt ? who(vt, b, sizeof b) : "?",
           who(__builtin_return_address(0), c, sizeof c), GetTickCount() - t_start);
    real_RegisterCallback(cb, id);
    wrap_callback(cb, size);
}

static void __cdecl hook_UnregisterCallback(void *cb) { real_UnregisterCallback(cb); }

static void __cdecl hook_RegisterCallResult(void *cb, unsigned long long call)
{
    void **vt = cb ? *(void ***)cb : NULL;
    real_RegisterCallResult(cb, call);
    wrap_callback(cb, cb_size(vt));
}

static void __cdecl hook_UnregisterCallResult(void *cb, unsigned long long call) { real_UnregisterCallResult(cb, call); }

void steam_install(void)
{
    char v[8];
    t_start = GetTickCount();
    cst_set_runner(net1_run_line);   /* the auto-join of "Join game" / an invite runs "join lobby:<id> steam" through net1 */
    log_stats = !(r1_env("REV1_NOACH", v, sizeof v) && v[0] == '1');
    ach_block = r1_env("REV1_ACHBLOCK", v, sizeof v) && v[0] == '1';
    if (ach_block)
        log_stats = 1;
    real_Init = (SteamAPI_Init_t)r1_patch_iat("steam_api.dll", "SteamAPI_Init", (void *)hook_Init);
    real_RunCallbacks = (SteamAPI_RunCallbacks_t)r1_patch_iat("steam_api.dll", "SteamAPI_RunCallbacks", (void *)hook_RunCallbacks);
    real_ContextInit = (ContextInit_t)r1_patch_iat("steam_api.dll", "SteamInternal_ContextInit", (void *)hook_ContextInit);
    real_RegisterCallback = (RegisterCallback_t)r1_patch_iat("steam_api.dll", "SteamAPI_RegisterCallback", (void *)hook_RegisterCallback);
    real_UnregisterCallback = (UnregisterCallback_t)r1_patch_iat("steam_api.dll", "SteamAPI_UnregisterCallback", (void *)hook_UnregisterCallback);
    real_RegisterCallResult = (RegisterCallResult_t)r1_patch_iat("steam_api.dll", "SteamAPI_RegisterCallResult", (void *)hook_RegisterCallResult);
    real_UnregisterCallResult = (UnregisterCallResult_t)r1_patch_iat("steam_api.dll", "SteamAPI_UnregisterCallResult", (void *)hook_UnregisterCallResult);
}
