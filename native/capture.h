/*
 * capture.h - the second human of Revelations 1 (campaign co-op engine, stage 3 result moved into the main DLL).
 *
 * What it does: the game has one local player (a uPlayer with control type 1, kind 1) and an AI partner (control 2, kind 2). Every reader of the input
 * manager indexes the pad by the "current slot" sGamePad+0x870 (always 0 in play). capture.c lets a CHARACTER be played by pad slot 0 or 1:
 *   - the character is taken over (control type 1, kind 1, upper-body state machine control mode 1) the first time its unit is seen,
 *     again after every load / checkpoint / area change (units are recreated; nothing hooks unit creation),
 *   - every phase of that unit (and of its weapon units and of its state-machine unit) runs with sGamePad+0x870 = its pad slot, so move, run, aim,
 *     fire, reload, melee, weapon change, items and look are read from that slot by the game's own code,
 *   - the input of a pad slot is a source: native (the game polls XInput user <slot> itself), a remote record pushed by the network layer
 *     (published as the synthetic XInput user <slot>), or a script (timed records, the lab bot).
 *
 * Roles (set before or after the level loads): capture_bind(unit_slot, pad_slot) = "the character with slot field unit_slot is played on pad_slot".
 * unit_slot is the character's slot field (unit+0xE2C): 0 = the first character of the pair (Jill in Episode 1), 1 = the second (Parker).
 * capture_set_own(unit_slot) = "this machine's human plays that character": the engine's own-slot query (FUN_0087E1B0) answers it, which decides who the
 * engine creates as the human player when an episode / checkpoint loads, whose HUD is drawn, whose doors/events react; the camera follows.
 *
 * Host plays Jill:    bind(0, 0), bind(1, 1), set_source(1, REMOTE), own = default.
 * Client plays Parker: set_own(1) BEFORE the episode loads, bind(1, 0), bind(0, 1), set_source(1, REMOTE) (Jill is driven by the host's records).
 *
 * Raid guard: no hook is installed while a Raid scene / session is active (game mode != 0 or sCoopManager+0xF4 in {1,2}); hooks that exist are removed at
 * the next logic pass and everything the capture changed is put back. It re-installs by itself when the campaign is back.
 *
 * Threads: the capture_* calls may come from any thread (network thread included): they only set requests; inline hooks are installed / removed on the
 * game's logic thread between two frames (capture_frame, registered with lt.c).
 */
#ifndef CAPTURE_H
#define CAPTURE_H
#include "coop_input.h"

/* addresses (build 13298487); the offline test harness points them at fake memory */
typedef struct {
    unsigned gamepad_ptr;   /* [..] = sGamePad: +0x870 current slot */
    unsigned splayer_ptr;   /* [..] = sPlayer: four circular lists of units */
    unsigned gamesys_ptr;   /* [[..]+0x24]+0x20 = game mode (0 campaign, 1 Raid) */
    unsigned coopmgr_ptr;   /* [..] = sCoopManager: +0xF4 in {1,2} = Raid session */
    unsigned camroot_ptr;   /* [..] = sGameCamera: [+0xCE0] = uCameraManage, +0x74/+0x78 = target unit */
    unsigned fn_padData;    /* FUN_0088A470  PadData[k] builder (thiscall(this, k)) */
    unsigned fn_v8;         /* 0x6CCAB0 uCharaBase phase dispatcher, vtable slot 8 */
    unsigned fn_v9;         /* 0x6C9C30 slot 9 (-> update) */
    unsigned fn_v10;        /* 0x6C9C10 slot 10 */
    unsigned fn_think;      /* 0x6C9800 uCallThink phase (state machine tick) */
    unsigned fn_own;        /* 0x87E1B0 own slot (sCoopManager+0xF4 in {1,2} ? +0xF8 : 0) */
    unsigned view_idx_ptr;  /* 0x116C3E4 DAT_0116C3E4 "view being created" (action icons, message boxes ... take their view mask from it; 0 = no such feature, harness) */
} capture_addr_t;

enum { CAP_NATIVE = R1_SRC_PAD, CAP_REMOTE = R1_SRC_REMOTE, CAP_SCRIPT = R1_SRC_SCRIPT };
enum { CAP_BLOCK_NONE = 0, CAP_BLOCK_RAID = 1, CAP_BLOCK_BUILD = 2 /* the bytes of a hook point differ: not this build */ };

typedef struct {
    int hooks_on;         /* inline hooks installed */
    int blocked;          /* CAP_BLOCK_* */
    int own_on;           /* the own slot is overridden */
    unsigned own_slot;
    int camera_follow;
    struct {
        int enabled;      /* bound */
        int pad;          /* pad slot */
        unsigned unit;    /* captured unit now (0 = not seen yet / gone) */
        unsigned swaps;   /* pad-slot swaps done for it */
    } role[2];            /* index = unit slot field */
    struct {
        int kind;         /* CAP_NATIVE / CAP_REMOTE / CAP_SCRIPT */
        r1_input_t last;  /* last record published (remote / script) */
        unsigned seq;     /* sequence number of the last accepted remote record */
        unsigned accepted, stale, timeouts;   /* remote records accepted / dropped as old / neutralised by the dead-man timer */
        unsigned ts;      /* sender timestamp of the last accepted record (ms, sender clock) */
        unsigned age_ms;  /* ms since the last accepted record arrived (remote) */
        int script_playing;
    } src[2];             /* index = pad slot */
    unsigned captures;    /* units taken over so far */
    int game_mode;        /* [[gamesys]+0x24]+0x20 */
} capture_state_t;

int capture_init(void);   /* once, from probe_install (game thread): registers the per-pass callback, hooks nothing yet */
void capture_frame(void); /* per logic pass, game thread (registered by capture_init) */

/* roles */
void capture_bind(int unit_slot, int pad_slot);   /* hooks are installed on the next pass */
void capture_unbind(int unit_slot);               /* -1 = all roles (the own slot stays) */
void capture_set_own(int unit_slot);              /* -1 = engine default (0 offline) */
void capture_set_camera_follow(int on);           /* camera follows the own slot's character (default on) */

/* input sources */
void capture_set_source(int pad_slot, int kind);  /* CAP_NATIVE: the game polls XInput user <pad_slot> itself; CAP_REMOTE: records from capture_push_remote */
/* A remote record (network thread ok). seq: wrapping 32-bit counter of the sender, a record that is not newer than the last accepted one is dropped (counted as stale);
 * ts_ms: sender clock, kept for the caller's jitter / delay statistics. Returns 1 when accepted. The record becomes the state of the synthetic XInput user <pad_slot>. */
int capture_push_remote(int pad_slot, const r1_input_t *rec, unsigned seq, unsigned ts_ms);
void capture_set_latch(int ms);                   /* a newly pressed remote button / trigger stays pressed at least ms (default 40): a tap shorter than one game poll is not lost */
void capture_set_remote_timeout(int ms);          /* 0 = off. A remote slot that got no record for ms is set neutral (no stuck buttons when the link stalls) */

void capture_get_state(capture_state_t *st);
void capture_status_text(char *out, int size);

/* console: "cap <sub> ..." on the game thread; returns the answer in out */
void capture_cmd(const char *arg, char *out, int size);

/* Services for split.c (the second view's HUD): the own-slot hook may be requested without any role; a SCOPE makes the own-slot query answer a given slot for the calling
 * thread only (the other threads keep the real answer; up to 8 threads at once), the scope is closed with slot -1. capture_own_effective() = the slot the engine answers right now (override, else the
 * game's own rule). Raid: the hook is removed with all others, a scope then does nothing. */
void capture_set_view_index(int unit_slot, unsigned view);   /* while a character (and its weapons) runs a swapped section, DAT_0116C3E4 = view (0 = leave alone): per-view units it creates get that viewport */
void capture_ext_request(int on);
int capture_hooks_ready(void);
void capture_own_scope(int slot);
int capture_own_effective(void);

/* 1 while any hook of the module is installed or requested (plug-ins skip their own capture hooks then) */
int capture_busy(void);

/* tests: replace the address table before capture_init (offline harness) */
void capture_test_set_addrs(const capture_addr_t *a);

#endif
