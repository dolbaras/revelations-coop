/*
 * lobby1.h - the co-op lobby of Revelations 1 (stage 6): the model behind the "Co-Op Play" screen. Pure C, no Windows and no game dependencies:
 * menu.c (game glue), overlay.c (drawing) and the offline test (tests/test_lobby1.c) use the same code.
 *
 * What the screen does: it is a settings screen. It arms the co-op (local split screen: two humans, one PC) and says who plays whom:
 *   - player 1 plays the LEAD character (unit slot 0, the one a single player controls) or the PARTNER (unit slot 1, normally the AI companion),
 *   - player 2 takes over the other one,
 *   - layout of the two views, which gamepad belongs to which player.
 * The player then starts the story in the stock menus (Continue / Load game / New game): the save decides the section and its pair of characters.
 * The model never draws anything: l1_view_get() fills a flat description which the overlay renders. Input arrives as events.
 */
#ifndef LOBBY1_H
#define LOBBY1_H

#include <stddef.h>

/* ---- localised texts (lobby1_text.c): own strings in 13 languages ---- */
enum l1_text {
    L1T_TITLE, L1T_SUBTITLE, L1T_COOP, L1T_ON, L1T_OFF, L1T_P1PLAYS, L1T_P2PLAYS, L1T_LEAD, L1T_PARTNER, L1T_LAYOUT, L1T_SIDE, L1T_TOPBOTTOM, L1T_PADS,
    L1T_PADS_NORMAL, L1T_PADS_SWAPPED, L1T_PADS_FOUND, L1T_DONE, L1T_HINT, L1T_ST_ON, L1T_ST_OFF, L1T_WARN_NOPAD, L1T_WARN_ONEPAD, L1T_PLAYER1, L1T_PLAYER2,
    L1T_PAD1, L1T_PAD2, L1T_NOTE_SAVE, L1T_NOTE_SOLO, L1T_HINT_TITLE, L1T_BANNER_ON, L1T_BANNER_OFF, L1T_NEXT_SECTION, L1T_RAID_NOTE,
    L1T_ROW_LABEL, L1T_ROW_HELP,   /* the native title menu row (menu.c): its label and its one-line help, put into the dead "menu_gamestart" / "memo_gamestart" messages */
    L1T_COUNT
};
enum { L1_LANGS = 13 };
/* language index: 0 eng, 1 fre, 2 ita, 3 ger, 4 spa, 5 jpn, 6 rus, 7 pol, 8 dut, 9 por (Portugal), 10 ptB (Brazil), 11 chS, 12 chT */
extern const char *const l1_lang_codes[L1_LANGS];      /* message file suffixes of the game: "eng", "fre", ... */
extern const char *const l1_lang_names[L1_LANGS];      /* English names */
const char *l1_text(int id, int lang);                 /* UTF-8, falls back to English, never NULL */
int l1_lang_from_steam(const char *steam_name);        /* "english", "french", "schinese", "brazilian" ... -> index; -1 unknown */
int l1_lang_from_langid(unsigned id);                  /* Windows LANGID (GetUserDefaultLangID, what the game itself reads) -> index; -1 = not a game language */
int l1_lang_from_code(const char *code);               /* "rus" -> 6; -1 unknown */
/* words of the stock menus that the texts quote ("$1" Continue, "$2" Load Game, "$3" New Game, "$4" Campaign): read from the game's own Title_<lang>.gmd by menu.c, built-in fallback per language */
void l1_set_stock_words(const char words[4][64]);
void l1_expand(const char *tmpl, int lang, char *out, size_t size);

/* ---- settings (persisted by menu.c) ---- */
enum { L1_LEAD = 0, L1_PARTNER = 1 };
enum { L1_LAYOUT_SIDE = 0, L1_LAYOUT_TOPBOTTOM = 1 };
typedef struct {
    int armed;           /* co-op is on: the next campaign start is a two human game */
    int role;            /* what player 1 plays: L1_LEAD / L1_PARTNER */
    int layout;          /* L1_LAYOUT_* */
    int pads_swapped;    /* 0: player 1 on pad slot 0, player 2 on pad slot 1; 1: the other way round */
} l1_settings;

/* what the lobby knows from the outside */
typedef struct {
    int lang;               /* 0..12 */
    int pads_connected;     /* XInput users 0 and 1 connected right now: 0..2 */
    char lead[40];          /* name of the lead character of the next / current section ("" = unknown: the Episode 1 pair is assumed) */
    char partner[40];
    int section_known;      /* the names come from a loaded section (live) */
    int in_game;            /* the lobby is opened from the pause menu / in a section (not used yet: the title is the entry) */
} l1_env;

/* ---- input ---- */
enum { L1E_NONE = 0, L1E_UP, L1E_DOWN, L1E_LEFT, L1E_RIGHT, L1E_OK, L1E_BACK, L1E_CLICK, L1E_HOVER };
typedef struct { int kind; int row; } l1_event;

/* ---- what to draw ---- */
enum { L1V_NORMAL = 0, L1V_GREY = 1, L1V_INFO = 2 };
enum { L1S_INFO = 0, L1S_WARN = 1, L1S_OK = 2 };
#define L1_ROWS 8
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
    char note[300];
    char note2[300];
} l1_view;

enum { L1R_NONE = 0, L1R_CLOSE };

/* ---- raw input -> events (lobby1_input.c): keyboard keys and two gamepads, with auto repeat; the key / button that opened the screen must be released first ---- */
typedef struct {
    unsigned char up, down, left, right, enter, space, esc, f6;   /* keyboard, 1 = down */
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

typedef struct l1 l1_t;
l1_t *l1_new(void);
void l1_free(l1_t *l);
void l1_open(l1_t *l, const l1_settings *s, const l1_env *env);   /* the screen opens */
void l1_set_env(l1_t *l, const l1_env *env);                      /* names / pads changed while open */
void l1_event_in(l1_t *l, const l1_event *e);
void l1_view_get(l1_t *l, l1_view *v);
void l1_settings_get(l1_t *l, l1_settings *s);
int l1_take_request(l1_t *l);                                     /* L1R_* once */
int l1_changed(l1_t *l);                                          /* 1 once after the settings changed (persist them) */

/* resolved assignment for the capture / split modules: unit slots of the two humans, their pad slots (player 1 is always on pad slot 0: the game's menus read slot 0 as "the local human")
 * and whether the two PHYSICAL gamepads are exchanged at the XInput level (testmode_pad_swap_base): that is how "player 1 uses gamepad 2" is done. */
typedef struct { int unit1, unit2, pad1, pad2, swap; } l1_assign;
void l1_assign_get(const l1_settings *s, l1_assign *a);

/* title-screen hint / banner line for the current settings */
void l1_hint_text(const l1_settings *s, const l1_env *env, char *out, size_t size);

/* settings file (key=value lines, UTF-8 or ASCII): returns 1 when the file existed */
int l1_ini_load(const char *path, l1_settings *s, int *lang_override);
int l1_ini_save(const char *path, const l1_settings *s, int lang_override);

/* character names of the section: unit class name ("uPl< ParkerLight >") -> person name index; -1 unknown. Order: 0 Jill 1 Chris 2 Parker 3 Keith 4 Jessica 5 Quint 6 Raymond */
int l1_person_from_class(const char *cls);
const char *l1_person_name(int person, int lang);                 /* name with the stock fallback (English) */
void l1_set_stock_names(const char names[7][48]);                  /* names read from the game's own Coop_<lang>.gmd (menu.c); NULL entries / empty = use the built-in */

#endif
