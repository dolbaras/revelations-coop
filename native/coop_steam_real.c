/*
 * The real Steam platform for coop_steam.c: it drives the Steam interfaces THE GAME ITSELF holds, through their C++ vtables
 * (32-bit MSVC thiscall, the same way steam.c and net.c already read them). No second steam_api.dll is loaded, no Steam call of the game is
 * changed; our own calls only use interfaces the game's CSteamAPIContext already carries (user, friends, utils, matchmaking, networking)
 * plus two generic ones from the same ISteamClient (SteamNetworkingMessages002, SteamNetworkingUtils004).
 *
 *   CSteamAPIContext (SDK 1.3x, seen in the log of SteamInternal_ContextInit): [0] ISteamClient [1] ISteamUser019 [2] ISteamFriends015
 *   [3] ISteamUtils [4] ISteamMatchmaking009 [5] UserStats [6] Apps [7] MatchmakingServers [8] ISteamNetworking005 ...
 *   Slot numbers = n_user / n_friends / n_utils / n_mm / n_net in net.c, checked against the stage 2b/2c logs (CreateLobby(2,2), GetLobbyOwner, ...).
 *   MSVC returns CSteamID (a class with a constructor) through a hidden first pointer argument; 64-bit SteamAPICall_t come back in edx:eax.
 *
 * Message pipe: "msg" = ISteamNetworkingMessages (Valve's relay network, its own queues: the game never calls it), or "p2p" = the game's own
 * SteamNetworking005 on channel CST_P2P_CHANNEL (the game reads channels 905 and 0 only). msg is the default when the generic interface exists.
 * Broadcast callbacks (Join game, invites, session requests) are caught with callback objects registered through the game's SteamAPI_RegisterCallback
 * from the game thread (cst_real_tick); call results (lobby created / entered / list) are polled with ISteamUtils::GetAPICallResult.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_steam.h"

#if defined(_WIN32) && defined(__i386__)

#include <windows.h>

#define TC __attribute__((thiscall))
#define VT(o) (*(void ***)(o))
#define ULL(x) ((unsigned long long)(x))

/* ---------------------------------------------------------------------------------------- state */
static void *g_ctx;
static volatile int g_cb_kind0;                 /* kind of vtable slot 0 of the game's callback objects: 3 = Run(param,failure,call) (MSVC) */
static struct {
    void *client, *user, *friends, *utils, *mm, *net, *msgs, *nutils;
    int hUser, hPipe;
    int pipe_msg;
    char pipe_pref[8];
    void (__cdecl *reg)(void *, int);
    void (__cdecl *unreg)(void *);
} R;
static int g_ready;
static int g_want_cb, g_cb_done;
static int g_pipe_tried;   /* the generic interfaces (Messages, NetworkingUtils) are fetched at the first real use, not while the game merely idles */

void cst_real_note_context(void *ctx) { g_ctx = ctx; }
void cst_real_note_callback_layout(int kind) { if ((kind == 1 || kind == 3) && !g_cb_kind0) g_cb_kind0 = kind; }
void cst_real_want_callbacks(void) { g_want_cb = 1; }

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || (uintptr_t)p < 0x10000 || !VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}
static int is_code(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
static int iface_ok(const void *o)
{
    return o && readable(o, 4) && readable(VT(o), 4 * 8) && is_code(VT(o)[0]);
}

/* ---------------------------------------------------------------------------------------- events from the callbacks */
static CRITICAL_SECTION ring_cs;
static int ring_ready;
static cst_event ring[64];
static int r_head, r_tail;

static void ring_init(void)
{
    if (!ring_ready) InitializeCriticalSection(&ring_cs), ring_ready = 1;   /* called on the init path (one thread at a time under coop_steam's lock) */
}
static void ring_push(const cst_event *e)
{
    if (!ring_ready) return;
    EnterCriticalSection(&ring_cs);
    int n = (r_tail + 1) % 64;
    if (n != r_head) ring[r_tail] = *e, r_tail = n;
    LeaveCriticalSection(&ring_cs);
}
static int ring_pop(cst_event *e)
{
    int ok = 0;
    if (!ring_ready) return 0;
    EnterCriticalSection(&ring_cs);
    if (r_head != r_tail) *e = ring[r_head], r_head = (r_head + 1) % 64, ok = 1;
    LeaveCriticalSection(&ring_cs);
    return ok;
}

/* ---- our callback objects (layout of the SDK's CCallbackBase: vptr, flags byte, iCallback at +8) ---- */
typedef struct { void **vt; unsigned char flags; int id; } cbobj;
static cbobj cbs[8];
static void *cb_vt[3];
static const int cb_ids[] = {333, 337, 506, 1251, 1252, 1202, 1203};

static uint64_t ident_id(const uint8_t *p)   /* SteamNetworkingIdentity of type SteamID */
{
    uint64_t id = 0;
    if (*(const int *)p == 16 && *(const int *)(p + 4) == 8) memcpy(&id, p + 8, 8);
    return id;
}

static void cb_handle(int id, const uint8_t *pv)
{
    cst_event e;
    memset(&e, 0, sizeof e);
    switch (id) {
    case 333:   /* GameLobbyJoinRequested_t { CSteamID lobby; CSteamID friend } */
        e.type = CST_EV_JOIN_LOBBY;
        memcpy(&e.id, pv, 8), memcpy(&e.id2, pv + 8, 8);
        break;
    case 337:   /* GameRichPresenceJoinRequested_t { CSteamID friend; char connect[256] } */
        e.type = CST_EV_JOIN_RICH;
        memcpy(&e.id2, pv, 8);
        memcpy(e.text, pv + 8, 256);
        e.text[255] = 0;
        break;
    case 506:   /* LobbyChatUpdate_t: a member came or went */
        e.type = CST_EV_LOBBY_CHANGE;
        memcpy(&e.id, pv, 8);
        break;
    case 1251:  /* SteamNetworkingMessagesSessionRequest_t { SteamNetworkingIdentity } */
        e.type = CST_EV_SESSION_REQUEST;
        e.id2 = ident_id(pv);
        break;
    case 1252:  /* SteamNetworkingMessagesSessionFailed_t { SteamNetConnectionInfo_t }: the identity comes first */
        e.type = CST_EV_SESSION_FAILED;
        e.id2 = ident_id(pv);
        break;
    case 1202:  /* P2PSessionRequest_t { CSteamID } : the game's own Raid code gets these too; only meaningful with the p2p pipe */
        if (!g_pipe_tried || R.pipe_msg) return;
        e.type = CST_EV_SESSION_REQUEST;
        memcpy(&e.id2, pv, 8);
        break;
    case 1203:  /* P2PSessionConnectFail_t { CSteamID; u8 error } */
        if (!g_pipe_tried || R.pipe_msg) return;
        e.type = CST_EV_SESSION_FAILED;
        memcpy(&e.id2, pv, 8);
        break;
    default:
        return;
    }
    if (e.id2 || e.id) ring_push(&e);
}

static void TC cb_run_pv(cbobj *self, void *pv) { cb_handle(self->id, (const uint8_t *)pv); }
static void TC cb_run_full(cbobj *self, void *pv, int fail, uint64_t call)
{
    (void)fail, (void)call;
    cb_handle(self->id, (const uint8_t *)pv);
}
static int TC cb_size(cbobj *self)
{
    switch (self->id) {
    case 333: return 16;
    case 337: return 264;
    case 506: return 32;
    case 1251: return 136;
    case 1252: return 696;
    case 1202: return 8;
    case 1203: return 9;
    }
    return 8;
}

static void register_callbacks(void)
{
    if (g_cb_done || !R.reg) return;
    g_cb_done = 1;
    int first_is_full = g_cb_kind0 != 1;   /* MSVC lays overloaded virtuals out in reverse: slot 0 = Run(param, failure, call) */
    cb_vt[0] = first_is_full ? (void *)cb_run_full : (void *)cb_run_pv;
    cb_vt[1] = first_is_full ? (void *)cb_run_pv : (void *)cb_run_full;
    cb_vt[2] = (void *)cb_size;
    for (unsigned i = 0; i < sizeof cb_ids / sizeof cb_ids[0]; i++) {
        cbs[i].vt = cb_vt, cbs[i].flags = 0, cbs[i].id = 0;
        R.reg(&cbs[i], cb_ids[i]);
    }
    cn_log("steam: registered %d callback objects (Join game, invites, lobby changes, pipe sessions), vtable order %s", (int)(sizeof cb_ids / sizeof cb_ids[0]),
           first_is_full ? "Run(param,failure,call) first (MSVC)" : "Run(param) first");
}

void cst_real_tick(void)
{
    if (g_want_cb && g_ready && !g_cb_done) register_callbacks();
}

/* ---------------------------------------------------------------------------------------- call results */
typedef struct { uint64_t call; int kind; uint64_t arg; } pend_t;
static pend_t pend[8];
static int n_pend;
static DWORD t_chk;

static int add_pending(uint64_t call, int kind, uint64_t arg)
{
    if (!call || n_pend >= 8) return 0;
    pend[n_pend].call = call, pend[n_pend].kind = kind, pend[n_pend].arg = arg;
    n_pend++;
    return 1;
}

typedef unsigned char (TC *f_b_u64_pb)(void *, uint64_t, unsigned char *);
typedef unsigned char (TC *f_b_u64_p_i_i_pb)(void *, uint64_t, void *, int, int, unsigned char *);

static void check_pending(void)
{
    DWORD now = GetTickCount();
    if (!n_pend || now - t_chk < 15) return;
    t_chk = now;
    for (int i = 0; i < n_pend; i++) {
        unsigned char failed = 0;
        if (!((f_b_u64_pb)VT(R.utils)[11])(R.utils, pend[i].call, &failed)) continue;   /* IsAPICallCompleted */
        uint8_t buf[64];
        memset(buf, 0, sizeof buf);
        cst_event e;
        memset(&e, 0, sizeof e);
        int cbid = pend[i].kind == 1 ? 513 : pend[i].kind == 2 ? 504 : 510;
        int size = pend[i].kind == 1 ? 16 : pend[i].kind == 2 ? 20 : 4;
        unsigned char f2 = 0;
        int ok = ((f_b_u64_p_i_i_pb)VT(R.utils)[13])(R.utils, pend[i].call, buf, size, cbid, &f2) && !f2 && !failed;   /* GetAPICallResult */
        if (pend[i].kind == 1) {
            e.type = CST_EV_LOBBY_CREATED;
            e.result = ok ? *(int *)buf : 0;
            if (ok) memcpy(&e.id, buf + 8, 8);
        } else if (pend[i].kind == 2) {
            e.type = CST_EV_LOBBY_ENTER;
            e.id = ok ? *(uint64_t *)buf : pend[i].arg;
            e.result = ok ? *(int *)(buf + 16) : 0;
        } else {
            e.type = CST_EV_LOBBY_LIST;
            e.n = ok ? *(int *)buf : 0;
        }
        if (!ok) cn_log("steam: call result kind %d failed (io failure %d / %d)", pend[i].kind, failed, f2);
        ring_push(&e);
        pend[i] = pend[--n_pend];
        i--;
    }
}

/* ---------------------------------------------------------------------------------------- the platform */
static int rp_init(void *c, char *err, size_t esz)
{
    (void)c;
    if (g_ready) return 1;
    void **x = (void **)g_ctx;
    if (!x || !readable(x, 4 * 9)) {
        snprintf(err, esz, "the game has not created its Steam context yet (SteamInternal_ContextInit not seen): wait for the title screen");
        return 0;
    }
    R.client = x[0], R.user = x[1], R.friends = x[2], R.utils = x[3], R.mm = x[4], R.net = x[8];
    if (!iface_ok(R.client) || !iface_ok(R.user) || !iface_ok(R.friends) || !iface_ok(R.utils) || !iface_ok(R.mm) || !iface_ok(R.net)) {
        snprintf(err, esz, "the game's Steam interfaces look invalid (SteamAPI_Init failed? is Steam running?)");
        return 0;
    }
    if (!((unsigned char (TC *)(void *))VT(R.user)[1])(R.user)) {   /* BLoggedOn */
        snprintf(err, esz, "Steam is offline (not logged on)");
        return 0;
    }
    HMODULE sa = GetModuleHandleA("steam_api.dll");
    int (__cdecl *get_user)(void) = sa ? (int (__cdecl *)(void))GetProcAddress(sa, "SteamAPI_GetHSteamUser") : NULL;
    int (__cdecl *get_pipe)(void) = sa ? (int (__cdecl *)(void))GetProcAddress(sa, "SteamAPI_GetHSteamPipe") : NULL;
    R.reg = sa ? (void (__cdecl *)(void *, int))GetProcAddress(sa, "SteamAPI_RegisterCallback") : NULL;
    R.unreg = sa ? (void (__cdecl *)(void *))GetProcAddress(sa, "SteamAPI_UnregisterCallback") : NULL;
    if (!get_user || !get_pipe || !R.reg) {
        snprintf(err, esz, "the game's steam_api.dll lacks SteamAPI_GetHSteamUser / GetHSteamPipe / RegisterCallback");
        return 0;
    }
    R.hUser = get_user(), R.hPipe = get_pipe();
    ring_init();
    g_ready = 1;
    cn_log("steam: interfaces from the game's context: client %p user %p friends %p utils %p matchmaking %p networking %p", R.client, R.user, R.friends, R.utils, R.mm, R.net);
    return 1;
}

/* The two generic interfaces (SteamNetworkingMessages002 for the pipe, SteamNetworkingUtils004 for the relay network), at the first real use: hosting, joining, a search. */
static const char *modoff(const void *a, char *buf, size_t n)
{
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, 92);
        snprintf(buf, n, "%s+%lx", b ? b + 1 : name, (unsigned long)((const char *)a - (const char *)m));
    } else
        snprintf(buf, n, "%p", a);
    return buf;
}

/* the first `n` vtable entries must be code pointers, else the object is not what the header says (a call through garbage killed the first hardware try) */
static int slots_ok(const char *what, void *o, int n)
{
    char line[400] = "", b[80];
    int ok = o && iface_ok(o);
    if (o && ok) {
        void **vt = VT(o);
        snprintf(line, sizeof line, "vtable %s:", modoff(vt, b, sizeof b));
        for (int i = 0; i < n + 2 && readable(&vt[i], 4); i++) {
            size_t l = strlen(line);
            snprintf(line + l, sizeof line - l, " [%d]=%s", i, is_code(vt[i]) ? modoff(vt[i], b, sizeof b) : "not code");
            if (i < n && !is_code(vt[i])) ok = 0;
        }
    }
    cn_log("steam: %s object %p %s%s", what, o, line, o && !ok ? " -> NOT usable" : "");
    return ok;
}

static void ensure_pipe(void)
{
    if (g_pipe_tried || !g_ready) return;
    g_pipe_tried = 1;
    typedef void *(TC * generic_t)(void *, int, int, const char *);
    generic_t gen = (generic_t)VT(R.client)[12];   /* ISteamClient::GetISteamGenericInterface (steam.c's logging wrapper; passes through) */
    R.msgs = gen(R.client, R.hUser, R.hPipe, "SteamNetworkingMessages002");
    R.nutils = gen(R.client, R.hUser, R.hPipe, "SteamNetworkingUtils004");
    if (R.msgs && !slots_ok("SteamNetworkingMessages002", R.msgs, 6)) R.msgs = NULL;
    if (R.nutils && !slots_ok("SteamNetworkingUtils004", R.nutils, 3)) R.nutils = NULL;
    /* InitRelayNetworkAccess (utils slot 1) is not called: the first hardware try died right after it (return into the stack); the relay network starts on first use anyway.
       REV2_STEAM_RELAYINIT=1 calls it, with log lines before and after, to find out why. */
    char ri[8] = "";
    if (R.nutils && GetEnvironmentVariableA("REV2_STEAM_RELAYINIT", ri, sizeof ri) && ri[0] == '1') {
        cn_log("steam: calling NetworkingUtils004 slot 1 (InitRelayNetworkAccess) ...");
        ((void (TC *)(void *))VT(R.nutils)[1])(R.nutils);
        cn_log("steam: ... it returned");
    }
    char v[16] = "";
    GetEnvironmentVariableA("REV2_STEAM_PIPE", v, sizeof v);
    if (!R.pipe_pref[0]) snprintf(R.pipe_pref, sizeof R.pipe_pref, "%s", v);
    R.pipe_msg = R.msgs && strcmp(R.pipe_pref, "p2p") != 0;
    cn_log("steam: NetworkingMessages002 %p, NetworkingUtils004 %p: pipe %s", R.msgs, R.nutils, R.pipe_msg ? "msg (Steam networking messages on channel 21042)" : "p2p (the game's SteamNetworking005 on channel 21042)");
}

static uint32_t rp_now(void *c) { (void)c; return cn_now_ms(); }

static uint64_t rp_my_id(void *c)
{
    (void)c;
    uint64_t id = 0;
    ((void *(TC *)(void *, uint64_t *))VT(R.user)[2])(R.user, &id);
    return id;
}
static const char *rp_my_name(void *c)
{
    (void)c;
    return ((const char *(TC *)(void *))VT(R.friends)[0])(R.friends);
}
static int rp_poll(void *c, cst_event *ev)
{
    (void)c;
    check_pending();
    return ring_pop(ev);
}

/* ---- lobbies (ISteamMatchmaking009) ---- */
#define MM(i) VT(R.mm)[i]
static int rp_lobby_create(void *c, int type, int max)
{
    (void)c;
    ensure_pipe();
    return add_pending(((uint64_t (TC *)(void *, int, int))MM(13))(R.mm, type, max), 1, 0);
}
static int rp_lobby_join(void *c, uint64_t lobby)
{
    (void)c;
    ensure_pipe();
    return add_pending(((uint64_t (TC *)(void *, uint64_t))MM(14))(R.mm, lobby), 2, lobby);
}
static void rp_lobby_leave(void *c, uint64_t lobby)
{
    (void)c;
    ((void (TC *)(void *, uint64_t))MM(15))(R.mm, lobby);
}
static int rp_lobby_search(void *c, const char *k1, const char *v1, const char *k2, const char *v2, int max)
{
    (void)c;
    typedef void (TC * strfilter_t)(void *, const char *, const char *, int);
    ensure_pipe();
    if (k1) ((strfilter_t)MM(5))(R.mm, k1, v1, 0);                         /* AddRequestLobbyListStringFilter(key, value, equal) */
    if (k2) ((strfilter_t)MM(5))(R.mm, k2, v2, 0);
    ((void (TC *)(void *, int))MM(9))(R.mm, 3);                            /* AddRequestLobbyListDistanceFilter(worldwide) */
    ((void (TC *)(void *, int))MM(10))(R.mm, max);                         /* AddRequestLobbyListResultCountFilter */
    return add_pending(((uint64_t (TC *)(void *))MM(4))(R.mm), 3, 0);       /* RequestLobbyList */
}
static uint64_t rp_lobby_at(void *c, int i)
{
    (void)c;
    uint64_t id = 0;
    ((void *(TC *)(void *, uint64_t *, int))MM(12))(R.mm, &id, i);
    return id;
}
static int rp_lobby_set(void *c, uint64_t lobby, const char *k, const char *v)
{
    (void)c;
    return ((unsigned char (TC *)(void *, uint64_t, const char *, const char *))MM(20))(R.mm, lobby, k, v);
}
static int rp_lobby_get(void *c, uint64_t lobby, const char *k, char *out, size_t size)
{
    (void)c;
    const char *v = ((const char *(TC *)(void *, uint64_t, const char *))MM(19))(R.mm, lobby, k);
    out[0] = 0;
    if (!v || !readable(v, 1)) return 0;
    snprintf(out, size, "%s", v);
    return (int)strlen(out);
}
static uint64_t rp_lobby_owner(void *c, uint64_t lobby)
{
    (void)c;
    uint64_t id = 0;
    ((void *(TC *)(void *, uint64_t *, uint64_t))MM(35))(R.mm, &id, lobby);
    return id;
}
static int rp_lobby_count(void *c, uint64_t lobby)
{
    (void)c;
    return ((int (TC *)(void *, uint64_t))MM(17))(R.mm, lobby);
}
static uint64_t rp_lobby_member(void *c, uint64_t lobby, int i)
{
    (void)c;
    uint64_t id = 0;
    ((void *(TC *)(void *, uint64_t *, uint64_t, int))MM(18))(R.mm, &id, lobby, i);
    return id;
}
static int rp_lobby_limit(void *c, uint64_t lobby)
{
    (void)c;
    return ((int (TC *)(void *, uint64_t))MM(32))(R.mm, lobby);
}
static int rp_lobby_invite(void *c, uint64_t lobby, uint64_t user)
{
    (void)c;
    return ((unsigned char (TC *)(void *, uint64_t, uint64_t))MM(16))(R.mm, lobby, user);
}

/* ---- friends (ISteamFriends015) ---- */
#define FR(i) VT(R.friends)[i]
static int rp_is_friend(void *c, uint64_t id)
{
    (void)c;
    return ((int (TC *)(void *, uint64_t))FR(5))(R.friends, id) == 3;   /* k_EFriendRelationshipFriend */
}
static const char *rp_name_of(void *c, uint64_t id)
{
    (void)c;
    const char *n = ((const char *(TC *)(void *, uint64_t))FR(7))(R.friends, id);
    return n && readable(n, 1) ? n : "";
}
static int rp_friend_count(void *c)
{
    (void)c;
    return ((int (TC *)(void *, int))FR(3))(R.friends, 4);   /* k_EFriendFlagImmediate */
}
static uint64_t rp_friend_at(void *c, int i)
{
    (void)c;
    uint64_t id = 0;
    ((void *(TC *)(void *, uint64_t *, int, int))FR(4))(R.friends, &id, i, 4);
    return id;
}
static int rp_friend_game(void *c, uint64_t id, uint32_t *appid, uint64_t *lobby)
{
    (void)c;
    struct { uint64_t game; uint32_t ip; uint16_t port, qport; uint64_t lobby; uint64_t pad[2]; } info;   /* FriendGameInfo_t is 24 bytes */
    memset(&info, 0, sizeof info);
    if (!((unsigned char (TC *)(void *, uint64_t, void *))FR(8))(R.friends, id, &info)) return 0;
    *appid = (uint32_t)(info.game & 0xFFFFFF);
    *lobby = info.lobby;
    return 1;
}
static int rp_presence(void *c, const char *k, const char *v)
{
    (void)c;
    return ((unsigned char (TC *)(void *, const char *, const char *))FR(43))(R.friends, k, v && v[0] ? v : NULL);
}
static void rp_presence_clear(void *c)
{
    (void)c;
    ((void (TC *)(void *))FR(44))(R.friends);
}
static int rp_invite_dialog(void *c, uint64_t lobby)
{
    (void)c;
    ((void (TC *)(void *, uint64_t))FR(33))(R.friends, lobby);
    return 1;
}
static int rp_diag(void *c, char *out, size_t size)
{
    (void)c;
    ensure_pipe();
    char dg[8] = "";
    if (!R.nutils || !is_code(VT(R.nutils)[2]) || !GetEnvironmentVariableA("REV2_STEAM_DIAG", dg, sizeof dg) || dg[0] != '1') {
        snprintf(out, size, "relay network: not queried (REV2_STEAM_DIAG=1 asks NetworkingUtils004 slot 2; its first call killed the game once)");
        return 0;
    }
    cn_log("steam: calling NetworkingUtils004 slot 2 (GetRelayNetworkStatus) ...");
    uint8_t d[512];
    memset(d, 0, sizeof d);
    int a = ((int (TC *)(void *, void *))VT(R.nutils)[2])(R.nutils, d);   /* GetRelayNetworkStatus */
    cn_log("steam: ... it returned %d", a);
    snprintf(out, size, "relay network: availability %d (100 = current), config %d, any relay %d", a, ((int *)d)[0], ((int *)d)[2]);
    return a;
}

/* ---- the message pipe ---- */
static const char *rp_pipe_name(void *c)
{
    (void)c;
    if (!g_pipe_tried) {   /* not decided yet: what it will be */
        char v[16] = "";
        GetEnvironmentVariableA("REV2_STEAM_PIPE", v, sizeof v);
        return (R.pipe_pref[0] ? !strcmp(R.pipe_pref, "p2p") : !strcmp(v, "p2p")) ? "p2p" : "msg (if available)";
    }
    return R.pipe_msg ? "msg" : "p2p";
}

static void make_ident(uint8_t *b, uint64_t id)   /* SteamNetworkingIdentity of type SteamID: { int type=16; int cb=8; uint64 id; ... } (136 bytes) */
{
    memset(b, 0, 160);
    *(int *)b = 16, *(int *)(b + 4) = 8;
    memcpy(b + 8, &id, 8);
}

static int rp_pipe_send(void *c, uint64_t to, const void *data, size_t len, int reliable)
{
    (void)c;
    ensure_pipe();
    if (R.pipe_msg) {
        uint8_t id[160];
        make_ident(id, to);
        /* SendMessageToUser(identity&, data, len, flags, channel): reliable 8, no Nagle 1, auto restart of a broken session 32 */
        int r = ((int (TC *)(void *, const void *, const void *, unsigned, int, int))VT(R.msgs)[0])(R.msgs, id, data, (unsigned)len, (reliable ? 8 : 0) | 1 | 32, CST_MSG_CHANNEL);
        return r == 1;
    }
    /* SendP2PPacket(steamid, data, len, type, channel): 2 reliable, 1 unreliable without delay */
    return ((unsigned char (TC *)(void *, uint64_t, const void *, unsigned, int, int))VT(R.net)[0])(R.net, to, data, (unsigned)len, reliable ? 2 : 1, CST_P2P_CHANNEL);
}

static int g_idbase;   /* where SteamNetworkingIdentity sits in SteamNetworkingMessage_t: 12 (members packed to 4, seen in Jericho) or 16 */
static uint64_t msg_sender(const uint8_t *m)
{
    if (!g_idbase) {
        if (*(const int *)(m + 12) == 16 && *(const int *)(m + 16) == 8) g_idbase = 12;
        else if (*(const int *)(m + 16) == 16 && *(const int *)(m + 20) == 8) g_idbase = 16;
        else return 0;
        cn_log("steam: message sender identity at offset %d", g_idbase);
    }
    return ident_id(m + g_idbase);
}
static void msg_release(void *m)
{
    if (!g_idbase) return;
    void *fn = *(void **)((uint8_t *)m + g_idbase + 164);   /* m_pfnRelease: after identity (136), user data, time, number (3 x 8) and m_pfnFreeData (4) */
    if (is_code(fn)) ((void (__cdecl *)(void *))fn)(m);
}

static void *g_mq[16];
static int g_mn, g_mi;

static int rp_pipe_recv(void *c, uint64_t *from, void *buf, size_t cap, size_t *len)
{
    (void)c;
    ensure_pipe();
    if (R.pipe_msg) {
        for (int guard = 0; guard < 64; guard++) {
            if (g_mi >= g_mn) {
                g_mn = ((int (TC *)(void *, int, void **, int))VT(R.msgs)[1])(R.msgs, CST_MSG_CHANNEL, g_mq, 16);   /* ReceiveMessagesOnChannel */
                g_mi = 0;
                if (g_mn <= 0) { g_mn = 0; return 0; }
            }
            uint8_t *m = (uint8_t *)g_mq[g_mi++];
            const void *d = *(void **)m;
            int sz = *(int *)(m + 4);
            uint64_t who = msg_sender(m);
            int ok = who && sz > 0 && (size_t)sz <= cap && readable(d, (size_t)sz);
            if (ok) memcpy(buf, d, (size_t)sz), *from = who, *len = (size_t)sz;
            msg_release(m);
            if (ok) return 1;
        }
        return 0;
    }
    unsigned sz = 0;
    if (!((unsigned char (TC *)(void *, unsigned *, int))VT(R.net)[1])(R.net, &sz, CST_P2P_CHANNEL)) return 0;   /* IsP2PPacketAvailable */
    unsigned got = 0;
    uint64_t who = 0;
    if (!((unsigned char (TC *)(void *, void *, unsigned, unsigned *, uint64_t *, int))VT(R.net)[2])(R.net, buf, (unsigned)cap, &got, &who, CST_P2P_CHANNEL)) return 0;   /* ReadP2PPacket */
    *from = who, *len = got;
    return 1;
}

static int rp_pipe_accept(void *c, uint64_t who)
{
    (void)c;
    ensure_pipe();
    if (R.pipe_msg) {
        uint8_t id[160];
        make_ident(id, who);
        return ((unsigned char (TC *)(void *, const void *))VT(R.msgs)[2])(R.msgs, id);   /* AcceptSessionWithUser */
    }
    return ((unsigned char (TC *)(void *, uint64_t))VT(R.net)[3])(R.net, who);   /* AcceptP2PSessionWithUser */
}

static void rp_pipe_close(void *c, uint64_t who)
{
    (void)c;
    ensure_pipe();
    if (R.pipe_msg) {
        uint8_t id[160];
        make_ident(id, who);
        ((unsigned char (TC *)(void *, const void *))VT(R.msgs)[3])(R.msgs, id);   /* CloseSessionWithUser */
    } else   /* only OUR channel: the game's own session with that user (channels 0 / 905) stays untouched */
        ((unsigned char (TC *)(void *, uint64_t, int))VT(R.net)[5])(R.net, who, CST_P2P_CHANNEL);   /* CloseP2PChannelWithUser */
}

static int rp_pipe_state(void *c, uint64_t who, int *ping)
{
    (void)c;
    *ping = -1;
    ensure_pipe();
    if (R.pipe_msg) {
        uint8_t id[160], st[512];
        make_ident(id, who);
        memset(st, 0, sizeof st);
        int s = ((int (TC *)(void *, const void *, void *, void *))VT(R.msgs)[5])(R.msgs, id, NULL, st);   /* GetSessionConnectionInfo(identity, NULL, quick status) */
        if (((int *)st)[1] >= 0) *ping = ((int *)st)[1];
        return s == 3 ? CST_PS_UP : (s == 1 || s == 2) ? CST_PS_CONNECTING : (s == 4 || s == 5) ? CST_PS_FAILED : CST_PS_NONE;
    }
    uint8_t st[32];
    memset(st, 0, sizeof st);
    if (!((unsigned char (TC *)(void *, uint64_t, void *))VT(R.net)[6])(R.net, who, st)) return CST_PS_NONE;   /* GetP2PSessionState */
    return st[0] ? CST_PS_UP : st[1] ? CST_PS_CONNECTING : st[2] ? CST_PS_FAILED : CST_PS_NONE;
}

static const cst_plat real_plat = {
    "real", rp_init, rp_now, rp_my_id, rp_my_name, rp_poll,
    rp_lobby_create, rp_lobby_join, rp_lobby_leave, rp_lobby_search, rp_lobby_at, rp_lobby_set, rp_lobby_get, rp_lobby_owner, rp_lobby_count, rp_lobby_member, rp_lobby_limit,
    rp_lobby_invite, rp_is_friend, rp_name_of, rp_friend_count, rp_friend_at, rp_friend_game, rp_presence, rp_presence_clear, rp_invite_dialog, rp_diag,
    rp_pipe_name, rp_pipe_send, rp_pipe_recv, rp_pipe_accept, rp_pipe_close, rp_pipe_state};

const cst_plat *cst_real_platform(void) { return &real_plat; }

void cst_real_set_pipe(const char *name, char *out, size_t size)
{
    while (*name == ' ') name++;
    if (!*name) {
        snprintf(out, size, "pipe: %s (msg = Steam networking messages, p2p = the game's SteamNetworking005 on channel %d); NetworkingMessages002 %s", g_pipe_tried ? (R.pipe_msg ? "msg" : "p2p") : "not decided yet",
                 CST_P2P_CHANNEL, R.msgs ? "available" : g_pipe_tried ? "NOT available" : "unknown until the first use");
        return;
    }
    snprintf(R.pipe_pref, sizeof R.pipe_pref, "%s", name);
    if (g_pipe_tried) R.pipe_msg = R.msgs && strcmp(R.pipe_pref, "p2p") != 0;
    snprintf(out, size, "pipe %s (applies to sessions started from now on)", g_pipe_tried ? (R.pipe_msg ? "msg" : "p2p") : R.pipe_pref);
}

#else   /* 64-bit build (tests, the headless bot): no game process, no real Steam */

const cst_plat *cst_real_platform(void) { return NULL; }
void cst_real_note_context(void *ctx) { (void)ctx; }
void cst_real_note_callback_layout(int kind) { (void)kind; }
void cst_real_tick(void) {}
void cst_real_want_callbacks(void) {}
void cst_real_set_pipe(const char *name, char *out, size_t size)
{
    (void)name;
    snprintf(out, size, "no real Steam platform in this build");
}

#endif
