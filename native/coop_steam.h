/*
 * Steam backend of the co-op transport ("steam" in coop_net.h): a Steam lobby for rendezvous / invites / "Join game", and the
 * lobby members talk to each other through Steam's relay network. No IP address, no port forwarding.
 *
 * Layers (all in coop_steam.c):
 *   cst_plat   a small platform interface in plain C types (64-bit ids, strings, bytes). The real one drives the Steam
 *              interfaces the GAME ITSELF holds (steam_api.dll of the game, no second Steam API); the tests plug a mock universe
 *              in through cst_set_platform. Nothing else in the file knows a vtable.
 *   backend    cn_ops "steam": lobby with our own private tag, link handshake, data/ping/bye messages, disconnect detection.
 *   commands   `net steamhost | steamjoin | steamlobbies | steaminvite | steam ...` (cst_command, called by cs_command).
 *   hooks      called by steam.c in the proxy: the game's SteamInternal_ContextInit result, a tick on the game thread before
 *              SteamAPI_RunCallbacks, and a filter for the game's own callbacks (cst_swallow_game_callback).
 *
 * Design, traps and the proof that the game never sees our packets: docs/steam-transport.md, notes in the lab (stage4-steam).
 */
#ifndef COOP_STEAM_H
#define COOP_STEAM_H

#include <stddef.h>
#include <stdint.h>

#include "coop_net.h"
#include "coop_session.h"

/* ---- our private lobby tag: the game's Raid code searches/creates lobbies with OwnerId, Name, IsBehindNAT, SlotPublic*, SlotPrivate*,
 *      SearchKeyNum, SearchKey0..6, BinarySize, BinaryData; none of these keys is ever used here, and "r1c" is never used by Raid ---- */
#define CST_TAG_KEY "r1c"
#define CST_TAG_VAL "R1COOP1"            /* the final digit is the backend wire version */
#define CST_KEY_HOST "r1c_host"         /* lobby data: owner's SteamID64 (decimal) */
#define CST_KEY_NAME "r1c_name"         /* owner's persona name (ASCII) */
#define CST_KEY_MAX "r1c_max"           /* player limit */
#define CST_KEY_PHASE "r1c_phase"       /* "lobby" | "game" (informational) */
#define CST_KEY_BUILD "r1c_build"       /* mod build string */

/* The legacy ISteamNetworking005 P2P channel used when the pipe is "p2p". The game's own loops read channel 905 (0x389, QoS) and 0 (game
 * data) only (code at 0xBF6338/0xBF6389 and 0xC0B998/0xC0B9E9). */
#define CST_P2P_CHANNEL 21042
/* The ISteamNetworkingMessages channel used when the pipe is "msg" (the game never calls that interface). */
#define CST_MSG_CHANNEL 21042

/* ---------------------------------------------------------------------------------------- platform interface */
enum {
    CST_EV_NONE = 0,
    CST_EV_LOBBY_CREATED,    /* result (1 = ok), id = lobby */
    CST_EV_LOBBY_ENTER,      /* id = lobby, result = EChatRoomEnterResponse (1 = success) */
    CST_EV_LOBBY_LIST,       /* n = number of lobbies (read with lobby_at) */
    CST_EV_LOBBY_CHANGE,     /* id = lobby: members or data changed (hint) */
    CST_EV_JOIN_LOBBY,       /* GameLobbyJoinRequested_t: id = lobby, id2 = friend */
    CST_EV_JOIN_RICH,        /* GameRichPresenceJoinRequested_t: id2 = friend, text = connect string */
    CST_EV_SESSION_REQUEST,  /* a remote user wants to talk to us on the pipe: id2 */
    CST_EV_SESSION_FAILED    /* the pipe session with id2 failed */
};
typedef struct {
    int type, result, n;
    uint64_t id, id2;
    char text[264];
} cst_event;

enum { CST_PS_NONE = 0, CST_PS_CONNECTING = 1, CST_PS_UP = 2, CST_PS_FAILED = 3 };
enum { CST_LOBBY_PRIVATE = 0, CST_LOBBY_FRIENDS = 1, CST_LOBBY_PUBLIC = 2 };

typedef struct cst_plat {
    const char *name;
    int (*init)(void *c, char *err, size_t esz);                 /* once: resolve the interfaces; 1 = ready */
    uint32_t (*now)(void *c);                                    /* milliseconds */
    uint64_t (*my_id)(void *c);
    const char *(*my_name)(void *c);                             /* UTF-8 persona name, valid until the next call */
    int (*poll)(void *c, cst_event *ev);                         /* 1 = an event was written */
    /* lobbies: results arrive as events */
    int (*lobby_create)(void *c, int type, int max);
    int (*lobby_join)(void *c, uint64_t lobby);
    void (*lobby_leave)(void *c, uint64_t lobby);
    int (*lobby_search)(void *c, const char *key, const char *val, const char *key2, const char *val2, int max_results);
    uint64_t (*lobby_at)(void *c, int i);
    int (*lobby_set)(void *c, uint64_t lobby, const char *k, const char *v);
    int (*lobby_get)(void *c, uint64_t lobby, const char *k, char *out, size_t size);   /* length of the value, 0 = empty / unknown key */
    uint64_t (*lobby_owner)(void *c, uint64_t lobby);
    int (*lobby_count)(void *c, uint64_t lobby);
    uint64_t (*lobby_member)(void *c, uint64_t lobby, int i);
    int (*lobby_limit)(void *c, uint64_t lobby);
    int (*lobby_invite)(void *c, uint64_t lobby, uint64_t user);
    /* friends, presence */
    int (*is_friend)(void *c, uint64_t id);
    const char *(*name_of)(void *c, uint64_t id);
    int (*friend_count)(void *c);
    uint64_t (*friend_at)(void *c, int i);
    int (*friend_game)(void *c, uint64_t id, uint32_t *appid, uint64_t *lobby);          /* 1 = the friend is in a game */
    int (*presence)(void *c, const char *key, const char *value);                        /* value NULL or "" clears the key */
    void (*presence_clear)(void *c);
    int (*invite_dialog)(void *c, uint64_t lobby);
    int (*diag)(void *c, char *out, size_t size);                                         /* optional: one line about the relay network (NULL ok) */
    /* message pipe between Steam users: whole messages, reliable or not */
    const char *(*pipe_name)(void *c);
    int (*pipe_send)(void *c, uint64_t to, const void *data, size_t len, int reliable);   /* 1 = taken by Steam */
    int (*pipe_recv)(void *c, uint64_t *from, void *buf, size_t cap, size_t *len);        /* 1 = a message */
    int (*pipe_accept)(void *c, uint64_t who);
    void (*pipe_close)(void *c, uint64_t who);
    int (*pipe_state)(void *c, uint64_t who, int *ping_ms);                               /* CST_PS_*; ping -1 unknown */
} cst_plat;

/* The platform used by the next host / join / command: NULL = the real one (32-bit game process only). Tests only. */
void cst_set_platform(const cst_plat *p, void *ctx);

/* Options for the next host (set by the commands; plain `net host 0 steam` uses the defaults: friends only, up to 4 players). */
void cst_set_host_options(int lobby_type, int max_players);
/* Who the host talks to: 0 friends (default), 1 any lobby member, 2 only the ids added with cst_allow. */
void cst_set_policy(int policy);
void cst_allow(uint64_t id);

/* Last error text of the backend ("" none): why a join/host failed after it had started. */
const char *cst_last_error(void);

/* Console: `net steam...` commands of cs_command. Returns 0 when the word is not ours. */
int cst_command(cs_t *s, const char *cmd, const char *arg, char *out, size_t size);

/* ---- hooks for the proxy (steam.c); not used by the tests except where noted ---- */
void cst_note_context(void *steam_api_context);   /* the CSteamAPIContext* returned by the game's SteamInternal_ContextInit */
void cst_note_callback_layout(int slot0_kind);    /* 1: slot 0 of a callback vtable is Run(param); 3: Run(param, failure, call) (MSVC: 3) */
void cst_game_tick(void);                         /* game thread, before SteamAPI_RunCallbacks: registers our callbacks, pumps events, auto-join */
int cst_swallow_game_callback(int id, const void *pv);   /* 1 = the callback concerns our lobby: do not run the game's handler */
void cst_set_runner(int (*run)(const char *line, char *out, int size));   /* how the auto-join runs a console line (`join lobby:<id> steam`) */
int cst_friends_in_game(uint64_t *ids, char (*names)[40], int max);   /* rel010: Steam friends in Revelations with a lobby now (the lobby's friend row); 0 when Steam is not ready */
int cst_callbacks_wanted(void);                   /* rel010: 1 = the game tick asked for the callback objects (Join game, invites, session requests) */
uint64_t cst_take_pending_lobby(void);            /* a lobby from "Join game" / invite / +connect_lobby that nobody used yet (0 none) */

/* ---- internal: the real platform (coop_steam_real.c; stubs in a 64-bit build) ---- */
const cst_plat *cst_real_platform(void);          /* NULL when this process cannot talk to Steam */
void cst_real_note_context(void *ctx);
void cst_real_note_callback_layout(int slot0_kind);
void cst_real_tick(void);                         /* game thread: register the callbacks when wanted */
void cst_real_want_callbacks(void);
void cst_real_set_pipe(const char *name, char *out, size_t size);   /* "msg" | "p2p" | "" (shows) */

/* tests: forget every hub and option (a fresh universe); cst_set_threaded(0) = the commands start the transport without a network thread (the caller pumps) */
void cst_reset(void);
void cst_set_threaded(int on);

extern const cn_ops cn_ops_steam;

#endif
