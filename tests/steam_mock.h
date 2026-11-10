/*
 * A mock Steam universe for the offline tests of the Steam backend (coop_steam.c): users with ids and names, a friend matrix, lobbies with
 * types / limits / data / owner migration, rich presence, invites, "Join game" clicks, and a message pipe between users with delay, loss,
 * duplication, reordering of unreliable messages, session requests that must be accepted, and a user that dies without saying goodbye.
 * It follows the real semantics we rely on (lobby types and responses, hidden friends-only lobbies, session accept), not Steam's internals.
 */
#ifndef STEAM_MOCK_H
#define STEAM_MOCK_H

#define MOCK_APP 222480u   /* the app the mock's friends play: Rev1 (lm6: coop_steam.c had kept Rev2's 287290) */
#include "../native/coop_steam.h"

typedef struct mock_user mock_user;

const cst_plat *mock_plat(void);
void mock_reset(void);                                   /* an empty universe, virtual time 0, no faults */
mock_user *mock_add_user(const char *name);
uint64_t mock_id(const mock_user *u);
void mock_befriend(mock_user *a, mock_user *b);
void mock_set_time(uint32_t ms);
uint32_t mock_time(void);
void mock_set_faults(int loss_pct, int dup_pct, int reorder_pct, int delay_ms, int jitter_ms, uint32_t seed);   /* unreliable messages; reliable only delayed */
void mock_set_implicit_accept(int on);                   /* sending to someone who asked first accepts his session (Steam does this) */
void mock_set_init_fail(mock_user *u, const char *why);  /* init() of this user fails (Steam offline) */

/* inspection */
const char *mock_lobby_get(uint64_t lobby, const char *key);   /* "" when unset */
int mock_lobby_count(uint64_t lobby);
int mock_lobby_limit(uint64_t lobby);
int mock_lobby_type(uint64_t lobby);
uint64_t mock_lobby_owner(uint64_t lobby);
int mock_lobby_exists(uint64_t lobby);
uint64_t mock_owned_lobby(const mock_user *u);              /* the lobby this user owns (0 none) */
int mock_lobby_has_key_like(uint64_t lobby, const char *prefix);   /* any data key starting with prefix */
const char *mock_presence(const mock_user *u, const char *key);
int mock_sessions_requested(const mock_user *u);             /* session requests this user got */
uint32_t mock_sent(const mock_user *u), mock_received(const mock_user *u);

/* actions of the world */
uint64_t mock_foreign_lobby(mock_user *owner, int type, int raid_like);   /* a lobby of another game (raid_like: Raid's keys) */
void mock_click_join_lobby(mock_user *u, uint64_t lobby, mock_user *from);   /* the user accepted an invite: GameLobbyJoinRequested_t */
void mock_click_join_rich(mock_user *u, mock_user *friend_, const char *connect);   /* GameRichPresenceJoinRequested_t */
void mock_kill(mock_user *u);                            /* the process died: nothing is sent or received any more, the lobby slot stays */
void mock_leave_all(mock_user *u);                       /* the user leaves every lobby (Steam went away) */
void mock_blackhole(mock_user *a, mock_user *b);         /* packets between them vanish, the pipe state still says up (a silent link) */
void mock_break_link(mock_user *a, mock_user *b);        /* packets between them vanish and the pipe state says failed */
void mock_set_in_game(mock_user *u, int in_game);

#endif
