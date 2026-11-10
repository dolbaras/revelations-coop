/*
 * net1.h - the network core of the Revelations 1 co-op mod (stage 4, B1): ONE link (lobby_link.c: the coop session over ENet) shared by the title
 * lobby (menu.c, Start / Disconnect / ready rows) and the test console (`net host|join <ip[:port]>|status|stop ...`). Whoever starts it, the link
 * lives until net1_stop (or a lost connection): closing the lobby does not touch it, and a lobby that is opened later shows the state of a link
 * started from the console. The game thread drives it: net1_frame once per logic frame (session notifications, ready flags, the automatic start
 * of the session when both players are ready), net1_cmd for the console (probe.c runs `net` on the game thread).
 *
 * Game independent apart from Windows: the proxy configures it (the real exe tag, the log sink), the offline tests configure it with the "sim"
 * transport and a virtual clock (tests/test_netcore.c).
 */
#ifndef NET1_H
#define NET1_H

#include <stddef.h>
#include <stdint.h>

#include "lobby_link.h"

typedef struct {
    const char *transport;          /* NULL = "enet" */
    int threaded;                   /* the transport has its own thread (the game); tests: 0 and net1_frame pumps */
    const char *exe_sha;            /* SHA-256 of rerev.exe as hex (the first 16 digits are the exe_tag the hosts compare); NULL / bad = zeros = not checked */
    const char *name;               /* the name the other player sees; NULL = the REV1_NAME variable, else the computer name */
    void (*log)(const char *line);  /* log sink for the net module and the transport; NULL = none */
    uint32_t (*clock)(void *);      /* NULL = the real clock */
    void *clock_user;
} net1_cfg;

/* before the first use (a second call replaces the configuration for a link created later) */
void net1_configure(const net1_cfg *c);
/* the first 8 bytes of a SHA-256 given as hex (16+ digits, either case): 1 = ok, tag filled; 0 = not a hash, tag zeroed */
int net1_parse_exe_tag(const char *hex, uint8_t tag[8]);

lk_t *net1_link(void);                                  /* the link (created at the first call); NULL when the session object cannot be made */
lk_t *net1_link_peek(void);                             /* the link when it exists, else NULL */
void net1_attach_lobby(l1_t *lobby);                    /* the lobby model shows the link (also a link that was started before; NULL = detach); safe to call every time the lobby opens */
/* start / stop the link (mode L1N_HOST: target ignored, port 0 = the default; L1N_JOIN: target "ip[:port]"). 1 = started; err says why not. */
int net1_start(int mode, const char *target, int port, char *err, size_t esz);
void net1_stop(void);
int net1_take_autojoin(void);                            /* rel010: 1 once after "Join game" / an invite started a Join link (the menu opens the co-op screen) */
int net1_session_active(void);                          /* 1 = a link is running and the session has STARTED (roles final): the network game is on (guards1_game.c switches its patches on it) */
void net1_frame(void);                                  /* once per logic frame, game thread */
void net1_own_addr(char *out, size_t size);             /* host: "ip:port" of this PC; "" otherwise */
void net1_cmd(const char *args, char *out, size_t size);   /* the console: net host [port] | join <ip[:port]> | status | stop | ready [0|1] | role lead|partner | set <k> <v> ... | start [force] | auto on|off | load [area] | done */
void net1_shutdown(void);                               /* frees the link (tests, process exit) */

/* lobby-must (product identity, set by menu.c after net1_configure): the mod version sent in HELLO / WELCOME / REFUSE (the other PC refuses another one), the name the partner
 * sees (persona(): the Steam persona name; NULL / "" = the localised "Player 1" / "Player 2" by the side; never the computer or the Windows user name: personal data) and the
 * product rule "no rejoin after the start" (REV1_REJOIN=1 keeps the lab's rejoin). Takes effect for a link created after the call (and the name at every start). */
void net1_set_identity(const char *(*persona)(void), const char *mod_version, int no_rejoin);
void net1_set_fallback(const char *(*fb)(int side));   /* the name without a persona, in this PC's language (side 0 host, 1 guest); NULL = English */
int net1_set_way(const char *way);                     /* "steam" | "ip" (= "enet") | "sim": the transport of the next start; 1 = known */
const char *net1_way(void);                            /* "steam" | "ip" */
int net1_steam_target_ok(const char *t);              /* a SteamID64, lobby:<id> or pending */
int net1_run_line(const char *line, char *out, int size);   /* coop_steam's auto-join: "join <target> [steam]" */
int net1_kick(void);                                    /* host in the lobby: remove the partner (1 = done) */
/* ep1-flow E12: the session flags byte the host publishes before the START (cp_session.flags: bit 0 the downed / revive rule, bits 1..2 the bleed-out preset);
 * default 1 = on, 60 s; menu.c sets the lobby row's value; REV1_DOWNED=0|1 / REV1_DOWNED_BLEED=30|60|120|0 override it (lab). */
void net1_set_session_flags(int flags);
int net1_session_flags(void);                           /* the byte as it will be published (env override applied) */
void net1_name_for(int mode, char *out, size_t size);   /* the name this PC sends when it starts in that mode (tests, status) */

#endif
