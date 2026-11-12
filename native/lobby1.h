/*
 * lobby1.h - the co-op lobby of Revelations 1 (stage 6): the model behind the "Co-Op Play" screen. Pure C, no Windows and no game dependencies:
 * menu.c (game glue), overlay.c (drawing) and the offline test (tests/test_lobby1.c) use the same code.
 *
 * What the screen does: it is a settings screen. It arms the co-op (local split screen: two humans, one PC) and says who plays whom:
 *   - player 1 plays the LEAD character (unit slot 0, the one a single player controls) or the PARTNER (unit slot 1, normally the AI companion),
 *   - player 2 takes over the other one,
 *   - layout of the two views, which gamepad belongs to which player.
 *   - (with env.net_rows) the network: Off / Host / Join, the address to join, Start / Disconnect, and "I am ready" once the link is up; the two player cards
 *     then show each player's state (the game's "Ready" / "Waiting"); when both are ready the host's link starts the session, and only then does the title
 *     menu's Campaign start the story (menu.c: the ready gate, both PCs).
 * The player then starts the story in the stock menus (Continue / Load game / New game): the save decides the section and its pair of characters.
 * The model never draws anything: l1_view_get() fills a flat description which the overlay renders. Input arrives as events.
 */
#ifndef LOBBY1_H
#define LOBBY1_H

#include <stddef.h>

typedef struct l1 l1_t;

/* ---- localised texts (lobby1_text.c): own strings in 13 languages ---- */
enum l1_text {
    L1T_TITLE, L1T_SUBTITLE, L1T_COOP, L1T_ON, L1T_OFF, L1T_P1PLAYS, L1T_P2PLAYS, L1T_LEAD, L1T_PARTNER, L1T_LAYOUT, L1T_SIDE, L1T_TOPBOTTOM, L1T_PADS,
    L1T_PADS_NORMAL, L1T_PADS_SWAPPED, L1T_PADS_FOUND, L1T_DONE, L1T_HINT, L1T_ST_ON, L1T_ST_OFF, L1T_WARN_NOPAD, L1T_WARN_ONEPAD, L1T_PLAYER1, L1T_PLAYER2,
    L1T_PAD1, L1T_PAD2, L1T_NOTE_SAVE, L1T_NOTE_SOLO, L1T_HINT_TITLE, L1T_BANNER_ON, L1T_BANNER_OFF, L1T_NEXT_SECTION, L1T_RAID_NOTE,
    L1T_LEG_OK, L1T_LEG_BACK,   /* the key legend at the bottom of the lobby ("A  OK   B  Back") */
    L1T_ROW_LABEL, L1T_ROW_HELP,   /* the native title menu row (menu.c): its label and its one-line help, put into the dead "menu_gamestart" / "memo_gamestart" messages */
    /* network rows (shown only when env.net_rows is set; the link itself is in the network glue) */
    L1T_NET, L1T_NET_HOST, L1T_NET_JOIN, L1T_NET_ADDR, L1T_NET_OWN, L1T_NET_ADDR_HINT, L1T_NET_START, L1T_NET_STOP, L1T_NET_READY,
    L1T_NET_ST_OFF, L1T_NET_ST_HOST, L1T_NET_ST_WAIT, L1T_NET_ST_JOIN, L1T_NET_ST_BADADDR, L1T_NET_ST_CONNECTING, L1T_NET_ST_CONN,
    L1T_NET_ST_WAITREADY, L1T_NET_ST_PEERREADY, L1T_NET_ST_BOTH, L1T_NET_ST_ERROR,
    /* lobby-must (first release 0.1.0): one exclusive Mode row, the network game's texts, refusal reasons, in-game strips. Our wording, not read by native speakers. */
    L1T_MODE_SPLIT, L1T_MODE_HOST, L1T_MODE_JOIN, L1T_SUBTITLE_NET, L1T_YOU_PLAY, L1T_NET_PEER, L1T_JOINING_AS, L1T_NET_ST_CONNECTING_TO, L1T_NET_ST_MARK,
    L1T_NET_ST_GO_HOST, L1T_NET_ST_GO_GUEST, L1T_NET_HOST_HINT, L1T_NOTE_SAVE_NET,
    L1T_REF_PROTO, L1T_REF_MODVER, L1T_REF_MODVER_OLD, L1T_REF_BUILD, L1T_REF_STARTED, L1T_REF_NOANSWER,
    L1T_WAIT_LOAD, L1T_PARTNER_SILENT, L1T_CONTINUES_ALONE, L1T_HOST_ENDED, L1T_TO_TITLE, L1T_KICK,
    L1T_NAME_P1, L1T_NAME_P2,   /* the fallback names when Steam gives no persona */
    L1T_NET_WAY, L1T_WAY_IP, L1T_WAY_STEAM, L1T_STEAM_HINT, L1T_NET_INVITE, L1T_NET_STEAMID, L1T_NET_STEAMID_HINT,   /* round 2: the Steam way */
    /* ep1-flow: the Downed / revive row (E12) and the end of Episode 1 (E5) */
    L1T_DOWNED,       /* row label "Downed / revive" */
    L1T_SECS,         /* "%s s": a bleed-out time */
    L1T_NO_LIMIT,     /* "No time limit": a downed player waits for the partner as long as it takes */
    L1T_END_DONE,     /* "Campaign complete! The co-op session has ended; your game goes on alone." */
    L1T_END_BEHIND,   /* "The host has reached the end of the campaign. The co-op session has ended." (this PC did not reach the end) */
    L1T_SOLO_WATCH,   /* ep1-solo E3: "For now %s goes on without you: you are watching until you meet again." (%s = the watched character) */
    L1T_NET_FRIEND,   /* rel010: the friend row of Join + Steam friends: "Friend hosting" */
    L1T_NET_NOFRIEND, /* rel010: its value while no Steam friend is in the game with a lobby */
    /* rev1 live 16 (the lead's decision: Continue keeps the roles of the co-op save; $1 = the game's Continue, $3 its New Game, the names the game's own) */
    L1T_CONT_ROLES,   /* the strip on both PCs when the host's Continue put the save's roles back: "$1 keeps the characters of the co-op save. Your character: %s." */
    L1T_NOTE_CONT_ROLES,   /* the host's room while its co-op save holds the roles: "$1 of the co-op save keeps its characters: you play %s, your partner %s. $3 ..." */
    /* allep1 (2026-11-10): every paired section of every episode is shared; these are the parts that cannot be (our wording, not read by native speakers) */
    L1T_SEC_BAD,       /* strip: this part is no section of the campaign (0-1, 0-2, 8-4 ...) and cannot be shared: the session has ended */
    L1T_NET_SEC_NOTE,  /* the host's room: its save continues in a part that network co-op cannot share */
    L1T_COUNT
};
/* the game's own strings (Coop_<lang>.gmd, the Raid mode's online texts, in all 13 languages) that the network lobby quotes: menu.c loads them for the running language
 * (l1_set_stock_text), the English built-in is the fallback. <USER> of the game's text becomes %s. */
enum l1_stock {
    L1K_FULL,          /* coop_error_mes_01 "The session you are trying to join is already full." */
    L1K_LOST,          /* coop_error_mes_09 "Connection to the other player has been terminated." */
    L1K_JOIN_FAILED,   /* coop_error_mes_12 "Failed to join session." */
    L1K_LEFT,          /* coop_guide_mes_10 "%s has left." */
    L1K_REMOVED,       /* coop_guide_mes_27 "You have been removed from the Lobby by the host." */
    L1K_JOINED,        /* coop_join "%s has joined." */
    L1K_READY,         /* coop_ready "%s is ready." */
    L1K_KICK_ASK,      /* coop_notice_kick "Do you want to kick %s?" */
    L1K_WAITING,       /* partner_waiting_info "Waiting for other player." */
    /* ready1 (2026-11-09): the Raid lobby's partner state words, shown on each player card of a network room (as Jericho's seats show READY) */
    L1K_PARTNER_READY, /* partner_ready "Ready" */
    L1K_PARTNER_WAIT,  /* partner_recruit "Waiting" (not ready yet) */
    /* ep1-flow: from other banks of the game (l1_stock_banks) */
    L1K_OFF,           /* option_<lang>.gmd game_setting_menu_01_02 "Off" (the options menu's word): the Downed / revive row's "rule off" */
    L1K_EPISODE1,      /* campaign_<lang>.gmd episode_01 "Episode 1": the strip at the end of Episode 1 */
    L1K_COUNT
};
extern const char *const l1_stock_labels[L1K_COUNT];    /* the GMD labels */
extern const char *const l1_stock_banks[L1K_COUNT];     /* the bank of each label: nativePC\msg\<bank>_<lang>.gmd ("Coop", "option", "campaign") */
int l1_stock_have(int id);                              /* 1 = the game's own text of the running language was loaded (else l1_stock gives the English built-in) */
void l1_set_stock_text(int id, const char *utf8);       /* a game text as the GMD holds it (tags, <USER>, line breaks): cleaned and kept; NULL / "" = back to the English built-in */
void l1_stock_clean(const char *gmd, int lang, char *out, size_t size);   /* tags dropped, <USER> -> %s, a stray % doubled, line breaks -> a space (none for Japanese / Chinese) */
const char *l1_stock(int id);                           /* the cleaned text (at most one %s), never NULL */
/* "%s" -> a1 then a2 (each at most once in order), "%%" -> "%"; nothing else is a format: safe with the game's text; output always valid UTF-8 and terminated */
void l1_fmt(char *out, size_t size, const char *tmpl, const char *a1, const char *a2);
enum { L1_LANGS = 13 };
/* language index: 0 eng, 1 fre, 2 ita, 3 ger, 4 spa, 5 jpn, 6 rus, 7 pol, 8 dut, 9 por (Portugal), 10 ptB (Brazil), 11 chS, 12 chT */
extern const char *const l1_lang_codes[L1_LANGS];      /* message file suffixes of the game: "eng", "fre", ... */
extern const char *const l1_lang_names[L1_LANGS];      /* English names */
const char *l1_text(int id, int lang);                 /* UTF-8, falls back to English, never NULL */
int l1_lang_from_steam(const char *steam_name);        /* "english", "french", "schinese", "brazilian" ... -> index; -1 unknown */
int l1_lang_from_langid(unsigned id);                  /* Windows LANGID (GetUserDefaultLangID, what the game itself reads) -> index; -1 = not a game language */
int l1_lang_from_game(unsigned mlanguage);             /* the game's own sMain.mLanguage (0..21) -> index (0 English for the languages it has no texts of); -1 out of range */
int l1_lang_from_code(const char *code);               /* "rus" -> 6; -1 unknown */
/* words of the stock menus that the texts quote ("$1" Continue, "$2" Load Game, "$3" New Game, "$4" Campaign): read from the game's own Title_<lang>.gmd by menu.c, built-in fallback per language */
void l1_set_stock_words(const char words[4][64]);
void l1_expand(const char *tmpl, int lang, char *out, size_t size);

/* ---- settings (persisted by menu.c) ---- */
enum { L1_LEAD = 0, L1_PARTNER = 1 };
enum { L1_LAYOUT_SIDE = 0, L1_LAYOUT_TOPBOTTOM = 1 };
/* network: what the two players do over the network (the link is run by the network glue, which reports it with l1_net_set_state) */
enum { L1N_OFF = 0, L1N_HOST = 1, L1N_JOIN = 2 };                                       /* settings.net_mode: "Off" / "Host" / "Join" */
enum { L1NS_IDLE = 0, L1NS_WAIT, L1NS_CONNECTING, L1NS_CONNECTED, L1NS_ERROR };          /* link state: not started, host waiting, connecting, connected, failed or lost */

typedef struct {
    int armed;           /* co-op is on: the next campaign start is a two human game */
    int role;            /* what player 1 plays: L1_LEAD / L1_PARTNER */
    int layout;          /* L1_LAYOUT_* */
    int pads_swapped;    /* 0: player 1 on pad slot 0, player 2 on pad slot 1; 1: the other way round */
    int net_mode;        /* L1N_*: not persisted, the game starts with the network off */
    char join_addr[48];  /* "host:port" typed for Join (the Steam way: the host's Steam ID); persisted */
    int way;             /* round 2: 0 = Direct IP, 1 = Steam friends (lobby + relay, invites, no port forwarding); persisted */
    int downed;          /* ep1-flow E12: the host's Downed / revive rule of a network session, L1D_* (0 = the default: on, 60 s); persisted */
} l1_settings;

/* ep1-flow E12: the Downed / revive row (network host; the guest sees the host's choice). The values cycle Off, 30 s, 60 s, 120 s, no time limit; 0 is the
 * default (60 s) so that a zeroed settings record means "on". The session flags byte (cp_session.flags, downed_logic.h): bit 0 = the rule, bits 1..2 = the
 * bleed-out preset (0 60 s, 1 30 s, 2 120 s, 3 never), the same bits as Revelations 2. */
enum { L1D_60 = 0, L1D_OFF = 1, L1D_30 = 2, L1D_120 = 3, L1D_NEVER = 4, L1D_N = 5 };
unsigned l1_downed_flags(int downed);                   /* L1D_* -> the session flags byte (Off = 0) */
int l1_downed_from_flags(unsigned flags);               /* the session flags byte -> L1D_* */
void l1_downed_text(int downed, int lang, char *out, size_t size);   /* "Off" (the game's word) / "30 s" / ... / "No time limit" */

#define L1_FRIENDS 8
/* what the lobby knows from the outside */
typedef struct {
    int lang;               /* 0..12 */
    int pads_connected;     /* XInput users 0 and 1 connected right now: 0..2 */
    char lead[40];          /* name of the lead character of the next / current section ("" = unknown: the Episode 1 pair is assumed) */
    char partner[40];
    int section_known;      /* the names come from a loaded section (live) */
    int in_game;            /* the lobby is opened from the pause menu / in a section (not used yet: the title is the entry) */
    int net_rows;           /* 1: the Mode row offers the network (product); 0: the screen is the local split-screen one, unchanged (REV1_NETROWS=0) */
    char own_addr[48];      /* this PC's address for Host ("192.168.0.5:28410"), set by the network glue; "" = not known yet */
    int alone_s;            /* host: seconds the link has been waiting without a partner (the internet hint comes after 30 s) */
    unsigned save_area;     /* the area of this PC's save (the checkpoint Continue loads; 0 = unknown): a network host is warned when it is outside Episode 1 */
    int n_friends;          /* rel010: Steam friends in the game with a lobby right now (the Join + Steam friends row), filled by the glue about once a second */
    char friend_name[L1_FRIENDS][40];
    char friend_id[L1_FRIENDS][24];   /* SteamID64 as digits: what the row puts into join_addr */
    int coop_role1;         /* rev1 live 16: the role recorded with this PC's co-op save + 1 (1 lead, 2 partner; 0 = no co-op save of a network game): a host's room says
                             * that Continue plays these characters */
} l1_env;

/* ---- input ---- */
enum { L1E_NONE = 0, L1E_UP, L1E_DOWN, L1E_LEFT, L1E_RIGHT, L1E_OK, L1E_BACK, L1E_CLICK, L1E_HOVER, L1E_CHAR, L1E_BACKSPACE };
typedef struct { int kind; int row; int ch; } l1_event;   /* ch: the typed character for L1E_CHAR (printable ASCII) */

/* ---- what to draw ---- */
enum { L1V_NORMAL = 0, L1V_GREY = 1, L1V_INFO = 2 };
enum { L1S_INFO = 0, L1S_WARN = 1, L1S_OK = 2 };
#define L1_ROWS 12              /* 7 local rows + 4 network rows at most */
typedef struct {
    char label[96];
    char value[128];
    int kind;           /* L1V_* */
    int action;         /* internal id, 0 for info rows */
} l1_row;
typedef struct {
    char who[48];       /* "PLAYER 1" */
    char chr[64];       /* character name (+ role) */
    char pad[64];       /* "Gamepad 1" / "Gamepad 2" */
    char ready[48];     /* ready1: a network room while the link is CONNECTED: this player's ready state (the game's "Ready" / "Waiting"), "" = no ready line */
    int ready_on;       /* 0 no ready line, 1 not ready, 2 ready */
} l1_card;
typedef struct {
    char title[96];
    char subtitle[200];
    l1_row rows[L1_ROWS];
    int nrows, sel;
    l1_card card[2];
    int cards_on;       /* draw the two player cards (co-op armed) */
    char status[240];
    int status_kind;    /* L1S_* */
    char hint[240];
    char note[400];
    char note2[400];
    char leg_ok[24], leg_back[24];   /* legend words */
    int pads;           /* gamepads connected: the legend shows round A / B buttons, else keyboard keys */
    char net_status[400];            /* network status line (network modes): what the link is doing and what to do next */
    int net_status_kind;             /* L1S_* */
} l1_view;

enum { L1R_NONE = 0, L1R_CLOSE, L1R_NET_START, L1R_NET_STOP, L1R_NET_KICK, L1R_NET_INVITE };   /* INVITE: the Steam way's "Invite a friend" */   /* NET_*: the glue starts / stops the link with the settings (settings.net_mode, join_addr), KICK removes the partner */

/* the exclusive mode of the Mode row (lobby-must): Off, local split screen, or one human per PC over the network. Stored as settings.armed (split) and settings.net_mode. */
enum { L1M_OFF = 0, L1M_SPLIT = 1, L1M_HOST = 2, L1M_JOIN = 3 };
int l1_mode_of(const l1_settings *s);                  /* L1M_*; the network wins when both are set (a stale armed=1 is never a split screen in a network game) */
void l1_mode_set(l1_settings *s, int mode);

/* why the link failed or ended: the reason the model shows instead of one generic text (lobby_link.c maps the session's notes to it) */
enum {
    L1RS_NONE = 0, L1RS_FAILED,      /* a local failure (no socket, bad address): the generic L1T_NET_ST_ERROR */
    L1RS_NOANSWER, L1RS_FULL, L1RS_STARTED, L1RS_PROTO, L1RS_MODVER, L1RS_MODVER_OLD, L1RS_BUILD, L1RS_REMOVED,
    L1RS_HOST_ENDED, L1RS_HOST_LOST,  /* a guest: the host closed the session / the link to the host is gone */
    L1RS_PARTNER_LEFT, L1RS_PARTNER_LOST,   /* a host: the partner left / was lost (an info line; the host keeps listening) */
    L1RS_COUNT
};
/* a1: L1RS_MODVER the host's version, L1RS_NOANSWER the address; a2: L1RS_MODVER / MODVER_OLD this PC's version; PARTNER_* the name in a1 */
void l1_net_set_reason(l1_t *l, int reason, const char *a1, const char *a2);
int l1_net_reason(const l1_t *l);
void l1_reason_text(int reason, int lang, const char *a1, const char *a2, char *out, size_t size);   /* the line for a reason (empty for NONE) */
void l1_net_set_peer(l1_t *l, const char *name);       /* the other player's name ("" = nobody / not known) */
void l1_net_set_started(l1_t *l, int started);         /* the session START happened: the roles are final, the status says what comes next */
void l1_net_set_host_flags(l1_t *l, int flags);        /* ep1-flow E12, a guest: the host's session flags byte as the session knows it (-1 = not known): the Downed / revive row shows it */

/* the in-game strip (OV_STRIP): one line about the partner / the session while the lobby is closed, drawn in levels and at the title */
enum {
    L1G_NONE = 0, L1G_WAIT_LOAD,     /* name: this PC has loaded and waits for the partner's load */
    L1G_SILENT,                      /* name, seconds: nothing from the partner */
    L1G_PARTNER_LEFT, L1G_PARTNER_LOST,   /* host: the partner is gone; the game goes on here */
    L1G_HOST_ENDED, L1G_HOST_LOST, L1G_REMOVED,   /* guest: the session is over, the game returns to the title */
    L1G_GUEST_HOLD,                  /* guest at the title after START: name = the host; the stock menu is held until the host's story start arrives */
    L1G_HOST_GO,                     /* host at the title after START: what to choose now */
    L1G_SOLO_WATCH,                  /* ep1-solo E3: the story switched this PC's character off (1-3): name = the watched character */
    L1G_READY_GATE,                  /* ready1: the title menu's Campaign was chosen while a network link runs and the session has not started: the story waits until both
                                      * players are ready (flags L1GF_*, name = the partner); says what is missing and where to mark ready */
    L1G_CONT_ROLES,                  /* rev1 live 16: the host's title Continue put the co-op save's roles back (both PCs): name = this PC's character now */
    L1G_CAMP_DONE,                   /* allep1: the campaign is complete (the last section's end), the session has ended, this PC's game goes on alone */
    L1G_CAMP_BEHIND,                 /* allep1: the host completed the campaign while this PC was behind: the session has ended, back to the title */
    L1G_SEC_BAD,                     /* allep1: this PC started a part that is no section of the campaign (0-1, 0-2, 8-4 ...): the session was closed */
    L1G_COUNT
};
enum { L1GF_CONNECTED = 1, L1GF_READY = 2, L1GF_PEER_READY = 4, L1GF_HOST = 8 };   /* l1_strip_in.flags of L1G_READY_GATE */
typedef struct { int kind; char name[40]; int seconds; int flags; } l1_strip_in;
void l1_strip_text(const l1_strip_in *in, int lang, char *out, size_t size, int *status_kind);

/* ---- raw input -> events (lobby1_input.c): keyboard keys and two gamepads, with auto repeat; the key / button that opened the screen must be released first ---- */
typedef struct {
    unsigned char up, down, left, right, enter, space, esc, f6;   /* keyboard, 1 = down */
    unsigned char backspace;                                      /* keyboard, 1 = down (auto repeat) */
    unsigned char typed[8];                                       /* printable ASCII typed since the last poll, 0-terminated (the glue fills it from the characters of the window) */
    unsigned short pad_buttons[2];                                /* XInput wButtons of physical users 0 and 1 (0 when not connected) */
    short lx[2], ly[2];                                           /* left stick */
} l1_raw;
typedef struct {
    int armed;                 /* the opening key has been released */
    unsigned char prev[24];    /* previous down state per input */
    unsigned next[24];         /* auto repeat deadlines */
    int init;
} l1_input;
void l1_input_reset(l1_input *in);                                 /* call when the screen opens */
/* open = the lobby is open. Returns the number of events written to ev (max entries); *want_open = 1 when the open gesture was made (F6 or the BACK button) while closed. */
int l1_input_poll(l1_input *in, const l1_raw *raw, unsigned now_ms, int open, l1_event *ev, int max, int *want_open);
/* typed characters of the game window (WM_CHAR, queued by the glue under its own lock): printable ASCII only, the rest is dropped */
typedef struct { unsigned char buf[32]; int head, count; } l1_typed;
void l1_typed_push(l1_typed *q, unsigned ch);                      /* 0x20..0x7E kept; a full queue drops the newest */
int l1_typed_take(l1_typed *q, unsigned char out[8]);              /* moves up to 7 characters into out, 0-terminated; returns the number moved */

l1_t *l1_new(void);
void l1_free(l1_t *l);
void l1_open(l1_t *l, const l1_settings *s, const l1_env *env);   /* the screen opens */
void l1_set_env(l1_t *l, const l1_env *env);                      /* names / pads changed while open */
void l1_event_in(l1_t *l, const l1_event *e);
void l1_view_get(l1_t *l, l1_view *v);
void l1_settings_get(l1_t *l, l1_settings *s);
int l1_take_request(l1_t *l);                                     /* L1R_* once */
int l1_changed(l1_t *l);                                          /* 1 once after the settings changed (persist them) */
void l1_net_set_state(l1_t *l, int state);                        /* L1NS_*: reported by the network glue; leaving CONNECTED clears both ready flags */
void l1_net_set_peer_ready(l1_t *l, int ready);                   /* the other player's ready flag, reported by the glue */
int l1_net_ready(const l1_t *l);                                  /* this player's ready flag (the glue sends it to the other player) */
int l1_net_state(const l1_t *l);                                  /* L1NS_* as last reported (for the glue and the tests) */
void l1_net_set_assigned(l1_t *l, int role);                       /* the role the host gave this (joined) player: L1_LEAD / L1_PARTNER, -1 = none; shown (and not changeable) while the Join link is CONNECTED */
void l1_net_set_ready(l1_t *l, int ready);                        /* the glue withdraws / sets this player's ready flag (the host reset it, or a console command); only while CONNECTED */
void l1_net_set_role(l1_t *l, int role);                          /* the glue sets settings.role (a console command); an ordinary setting, no change flag */
void l1_net_adopt(l1_t *l, int mode, const char *addr);           /* the link was started outside the lobby (console): show its mode (and the join address) */
int l1_addr_ok(const char *s);                                    /* "host[:port]": host = letters, digits, '.', '-'; port 1..65535 when given; 1 = ok */

/* resolved assignment for the capture / split modules: unit slots of the two humans, their pad slots (player 1 is always on pad slot 0: the game's menus read slot 0 as "the local human")
 * and whether the two PHYSICAL gamepads are exchanged at the XInput level (testmode_pad_swap_base): that is how "player 1 uses gamepad 2" is done. */
typedef struct { int unit1, unit2, pad1, pad2, swap; } l1_assign;
void l1_assign_get(const l1_settings *s, l1_assign *a);

/* title-screen hint / banner line for the current settings */
void l1_hint_text(const l1_settings *s, const l1_env *env, char *out, size_t size);

/* settings file (key=value lines, UTF-8 or ASCII): returns 1 when the file existed */
int l1_ini_load(const char *path, l1_settings *s, int *lang_override);
int l1_ini_save(const char *path, const l1_settings *s, int lang_override);

/* character names of the section: unit class name ("uPl< ParkerLight >") -> person name index; -1 unknown. Order: 0 Jill 1 Chris 2 Parker 3 Keith 4 Jessica 5 Quint 6 Raymond 7 Hunk (hunk1.h, built-in name) */
int l1_person_from_class(const char *cls);
const char *l1_person_name(int person, int lang);                 /* name with the stock fallback (English) */
void l1_set_stock_names(const char names[7][48]);                  /* names read from the game's own Coop_<lang>.gmd (menu.c); NULL entries / empty = use the built-in */

#endif
