/*
 * split.h - local split screen of Revelations 1 (two humans, one PC, two views), stage 5 result moved into the main DLL.
 *
 * What it does (notes\stage5.md): the engine already renders a viewport array. sCamera ([0x11E00B8]) owns 8 viewport objects; the scene pass runs once per ACTIVE viewport,
 * each with its own camera object, rect (a layout byte: 2 top / 3 bottom / 4 left / 5 right half), matrices, frustum and shadow view, and the aspect ratio of every camera
 * is set from the rect. The stock game switches on viewport 0 only (camera = the first uCameraManage) and also creates a second manager that the player code uses for the
 * character that is not the own slot. This module switches viewport 2 on with the second manager, sets the layout bytes and keeps them; it writes plain bytes once per logic
 * pass (before the frame) and puts everything back when the split ends. The HUD per view is a second set of the cockpit units (own slot answered as the other human
 * while its update phases run), drawn only in viewport 2.
 *
 * Rules (whole view = layout 0, viewport 2 off, resumes by itself): a cutscene or title camera on viewport 0, the pause menu / game over screen / item menu (GUI units that the
 * engine draws in every viewport), a Raid scene (the module never touches Raid: it ends the split and removes its HUD set).
 *
 * Who calls it:
 *   lobby (stage 6, local split screen):  capture_bind(0, 0); capture_bind(1, 1);  split_set_layout(...);  split_enable(1);   ... at the end split_enable(0).
 *   network (stage 4): one human per PC: split is off; both features never run together (the campaign has two characters only).
 * The calls only set requests, the work is done on the game's logic thread in split_frame (registered with lt.c), so any thread may call them.
 */
#ifndef SPLIT_H
#define SPLIT_H

/* addresses of build 13298487; the offline harness points them at fake memory */
typedef struct {
    unsigned camroot_ptr;   /* [..] = sGameCamera: viewports at +0x30 (stride 0x190), camera managers [+0xCE0] [+0xCE4], window rect +0xCB0 */
    unsigned splayer_ptr;   /* [..] = sPlayer: circular lists of units (the units by slot field +0xE2C) */
    unsigned gamesys_ptr;   /* [[..]+0x24]+0x20 = game mode (0 campaign) */
    unsigned coopmgr_ptr;   /* [..] = sCoopManager: +0xF4 in {1,2} = Raid session, +0xF8 own slot */
    unsigned cockpit_ptr;   /* [..] = sIDCockpit: +0x28 main cockpit manager, +0x2C minimap manager, +0x30 game over, +0x34 pause, +0x38 pause blur, +0x3C item menu */
    unsigned unitmgr_ptr;   /* [..] = sUnit: groups (first unit of group k at +0x2C+0x18k, next at unit+0x14) */
    unsigned fn_cp_alloc;   /* 0x7AFFB0  cdecl(size, align): uCockpitManagerMain factory */
    unsigned fn_cp_ctor;    /* 0x7AFFE0  fastcall(this) */
    unsigned fn_cp_kids;    /* 0x7B06D0  fastcall(manager): builds the ~20 children */
    unsigned fn_mm_alloc;   /* 0x7BA440  cdecl(size, align): uMiniMapManager factory */
    unsigned fn_mm_ctor;    /* 0x7BA470  fastcall(this) */
    unsigned fn_mm_kids;    /* 0x7BA4F0  fastcall(manager): builds the four map units */
    unsigned fn_register;   /* 0x9B5280  thiscall(sUnit, group, unit, 0, 0), ret 0x10 */
    unsigned alloc_table;   /* 0x11859D0: table of 64 allocator object pointers (0 = do not zero-fill the new set's blocks): the slot 7 of their vtables is wrapped during the build */
} split_addr_t;

enum { SPLIT_SIDE_BY_SIDE = 0, SPLIT_TOP_BOTTOM = 1 };
enum { SPLIT_BLOCK_NONE = 0, SPLIT_BLOCK_RAID = 1 };
enum { SPLIT_SUSP_NONE = 0, SPLIT_SUSP_CAMERA = 1 /* cutscene / title camera on viewport 0 */, SPLIT_SUSP_GUI = 2 /* pause, game over, item menu */, SPLIT_SUSP_SOLO = 3 /* the other human's character does not exist (a solo part of the story) */ };

typedef struct {
    int requested;         /* split_enable(1) was called */
    int active;            /* the viewports are switched on right now (requested, campaign, not suspended) */
    int blocked;           /* SPLIT_BLOCK_* */
    int suspended;         /* SPLIT_SUSP_* : whole view meanwhile */
    int layout;            /* SPLIT_SIDE_BY_SIDE / SPLIT_TOP_BOTTOM */
    int hud_set;           /* the second HUD set exists */
    int menu_exchange;     /* the second human's menu is open: pads exchanged, own slot flipped */
    unsigned passes, reasserted, suspensions, hud_builds;
} split_state_t;

int split_init(void);                  /* once, from probe_install (game thread): registers the per-pass callback; nothing is touched until split_enable(1) */
void split_frame(void);                /* per logic pass, game thread (registered by split_init) */

void split_enable(int on);             /* request the split screen on / off (off puts every byte back) */
void split_emergency_off(void);      /* any thread (console "splitkill"): clears the request and restores the viewport bytes even when the game thread is stuck */
void split_set_layout(int layout);     /* SPLIT_SIDE_BY_SIDE (default) or SPLIT_TOP_BOTTOM; applied at the next pass */
void split_set_hud(int on);            /* second HUD set on / off (default on); off = both views show the stock HUD of the first human */
void split_get_state(split_state_t *st);
int split_is_active(void);
unsigned split_second_minimap(void);  /* the second HUD set's uMiniMapManager, 0 = none */

/* console: "split <sub> ..." on the game thread */
void split_cmd(const char *arg, char *out, int size);
void split_status_text(char *out, int size);

/* tests: replace the address table before split_init (offline harness) */
void split_test_set_addrs(const split_addr_t *a);

#endif
