/*
 * mapptn1.h - the co-op partner on the HUD minimap of Revelations 1 (build 13298487): the pure part (byte checks, the stub, state, status text, the
 * decision models the tests compare), independent of Windows. Memory is reached through the mp1_mem table: the game glue (mapptn1_game.c) points it
 * at the process, the offline tests (tests/test_mapptn1.c, tests/test_mapptn1_x86.c) at buffers.
 *
 * What the stock game does (static, notes\minimap1.md): the HUD minimap is uMiniMapManager [sIDCockpit 0x1169120 +0x2C] with four units; uGUI_Map
 * (manager +0x48, update vf9 0x7B7B30) computes the map positions, uMapIcon (manager +0x4C, update vf9 0x7B95E0) places the icons from them.
 * uGUI_Map_vf9 asks for "the partner" (FUN_0041BC10: the first player unit whose slot is not the own slot) ONLY when the game mode
 * [[0x1169108]+0x24]+0x20 is 1 (Raid), and even then hides an AI partner (FUN_006A7C20: unit +0xE30 == 2). In the campaign it jumps over the
 * partner (0x7B7EE5 `jnz 0x7B7F28`: partner = 0 -> uGUI_Map +0x3B8 = 0 -> uMapIcon shows no partner icon). The campaign co-op (split screen and
 * network) therefore never showed the other human on the minimap.
 *
 * The patch (written only while a co-op game runs, mapptn1_game.c): the 9 bytes at 0x7B7EDE
 *     8b 42 24        mov eax,[edx+0x24]
 *     83 78 20 01     cmp dword ptr [eax+0x20],1
 *     75 41           jnz 0x7B7F28
 * become `jmp stub` + 4 x nop. The stub repeats the mode test (Raid keeps the stock path at 0x7B7EE7), and in the campaign asks FUN_0041BC10 for the
 * partner exactly as the Raid path does (thiscall on sPlayer [0x1169394], one stack argument = &[esp+0x13], ret 4) but without the AI hide, stores it
 * in the frame slot [esp+0x14] (local_5c of vf9) and continues at 0x7B7F2C, the stock code that computes the partner's map position (+0x3A8),
 * room / floor (+0x3B0) and icon type (+0x3B8: 1 in view, 2..5 at the edge, +5 on another floor). The own slot that FUN_0041BC10 compares with is the
 * mod's (capture.c), so the split's second HUD set (own slot flipped, split.c) gets the first human as ITS partner.
 *
 * The split's second HUD set (split.c) has a second uMiniMapManager, but uMapIcon_vf9 and the HUD-hidden test FUN_0079BB80 read "the minimap" through
 * [sIDCockpit+0x2C] (FUN_00849780), i.e. always the STOCK manager: the second view's icons were the first human's. split.c points that cell at the
 * second manager for the duration of the second manager's update phases (mp1_swap_*).
 */
#ifndef MAPPTN1_H
#define MAPPTN1_H
#include <stddef.h>
#include <stdint.h>

#define MP1_SITE        0x7B7EDEu   /* 9 bytes checked and written */
#define MP1_SITE_LEN    9
#define MP1_RAID_PATH   0x7B7EE7u   /* stock: mov ecx,[0x1169394] (the Raid partner block) */
#define MP1_NO_PARTNER  0x7B7F28u   /* stock: mov [esp+0x14],ebx (partner = 0) */
#define MP1_CONTINUE    0x7B7F2Cu   /* stock: mov edi,[esp+0x1c] (the code after the partner choice) */
#define MP1_PARTNER_FN  0x41BC10u   /* FUN_0041BC10 thiscall(sPlayer, char *out), ret 4: the first player unit whose slot is not the own slot */
#define MP1_SPLAYER     0x1169394u  /* cell: sPlayer */
#define MP1_COCKPIT     0x1169120u  /* cell: sIDCockpit (+0x2C the stock uMiniMapManager) */

enum { MP1_OFF = 0, MP1_ON, MP1_MISMATCH, MP1_ERROR };
enum { MP1_MODE_AUTO = 0, MP1_MODE_ON, MP1_MODE_OFF };

typedef struct {
    int (*read)(void *user, uint32_t addr, void *out, uint32_t n);
    int (*write)(void *user, uint32_t addr, const void *p, uint32_t n);
    uint32_t (*alloc)(void *user, uint32_t n);   /* executable + writable, lives for the whole process; 0 = failed */
    void (*log)(void *user, const char *line);   /* may be NULL */
    void *user;
} mp1_mem;

/* the addresses the stub uses (the game's; the x86 test points them at its own code) */
typedef struct {
    uint32_t raid_path, no_partner, cont, partner_fn, splayer_cell;
} mp1_targets;

typedef struct {
    int mode;                 /* MP1_MODE_* chosen by the console */
    int env;                  /* REV1_MAPPTN: -1 unset, 0 never, 1 always */
    uint32_t stub;            /* 0 = not built yet */
    int state;
    char why[96];
    unsigned applies, reverts, refusals;
} mp1_t;

extern const uint8_t mp1_site_orig[MP1_SITE_LEN];
extern const uint8_t mp1_raid_path_bytes[6];   /* 8b 0d 94 93 16 01 at 0x7B7EE7 */
extern const uint8_t mp1_no_partner_bytes[4];  /* 89 5c 24 14 at 0x7B7F28 */
extern const uint8_t mp1_continue_bytes[4];    /* 8b 7c 24 1c at 0x7B7F2C */
extern const uint8_t mp1_partner_fn_bytes[6];  /* 53 55 8b e9 8b 5d at 0x41BC10 */

void mp1_targets_game(mp1_targets *t);
int mp1_stub_build(uint8_t *out, uint32_t stub_addr, const mp1_targets *t);   /* returns the length (MP1_STUB_LEN) */
#define MP1_STUB_LEN 46
void mp1_site_build(uint8_t out[MP1_SITE_LEN], uint32_t stub_addr);          /* jmp stub + 4 x nop, placed at MP1_SITE */
void mp1_site_build_at(uint8_t out[MP1_SITE_LEN], uint32_t site_addr, uint32_t stub_addr);   /* the same for another address (x86 test) */

/* the switch: co-op = a split screen that is requested and not blocked (campaign), or a network session that has started; REV1_MAPPTN / console override */
int mp1_want(const mp1_t *g, int split_on, int net_active);

void mp1_init(mp1_t *g, int env);
int mp1_apply(mp1_t *g, const mp1_mem *m);    /* 1 = on now (already on counts), 0 = refused (g->why says why) */
int mp1_revert(mp1_t *g, const mp1_mem *m);   /* 1 = original bytes now */
int mp1_probe(const mp1_t *g, const mp1_mem *m, char *out, size_t size);
void mp1_status(const mp1_t *g, const mp1_mem *m, int split_on, int net_active, char *out, size_t size);
const char *mp1_state_name(int st);

/* ---- the decision models (what the minimap shows as "partner"; tests) ---- */
/* stock uGUI_Map_vf9: mode 1 (Raid) only, an AI partner (role 2) hidden */
static inline int mp1_stock_partner_shown(int game_mode, int partner_exists, int partner_role)
{
    return game_mode == 1 && partner_exists && partner_role != 2;
}
/* with the patch written (co-op): Raid unchanged, campaign: any partner unit FUN_0041BC10 returns */
static inline int mp1_fixed_partner_shown(int patched, int game_mode, int partner_exists, int partner_role)
{
    if (!patched || game_mode == 1)
        return mp1_stock_partner_shown(game_mode, partner_exists, partner_role);
    return partner_exists;
}

/* ---- the second HUD set of the split (split.c): the minimap cell [sIDCockpit+0x2C] during the second manager's update ---- */
/* begin: returns the value to put back afterwards, 0 = leave the cell alone (no cockpit, the stock manager is not the one we know, already pointing at us) */
static inline uint32_t mp1_swap_begin(uint32_t cell_now, uint32_t stock_mm, uint32_t second_mm)
{
    return (second_mm && cell_now && cell_now == stock_mm && cell_now != second_mm) ? cell_now : 0;
}
/* end: put `back` only if the cell still points at the second manager (the engine may have rebuilt the cockpit meanwhile: then it is left alone) */
static inline int mp1_swap_restore_ok(uint32_t back, uint32_t cell_now, uint32_t second_mm)
{
    return back && cell_now == second_mm;
}

/* ---- the map screen (BACK -> uSubMenuManager -> the 3D world map): a partner marker ----
 * The stock 3D map (`uGUI_WorldMapIcon`, vtable 0x10829B8, GUI id_bhr\10_cockpit\cockpit_3dmap_icon) has no partner marker at all: its load (vf5
 * 0x7BE470) takes parts 0-5 (+0x274 markers, part 0 cloned 14 times), part 6 (+0x4F0 markers, cloned 19 times), part 7 = the player (+0x774) and part 8 =
 * the objective (+0x794); FUN_007C10D0 fills the player slot from FUN_008D30D0 (the LOCAL player's position in world-map space). The co-op marker: a
 * clone of the player part (the stock way: FUN_00B9FC90 clone, FUN_00B9DBB0 add to the GUI), placed every update (vf8 0x7BED30, wrapped through its
 * vtable slot) with the stock placement FUN_007BE8B0 at FUN_008D30D0's answer for the PARTNER (the own slot answered by the partner for that call:
 * capture_own_scope), shown while the player marker is shown. */
#define MP1_WM_VTABLE   0x10829B8u
#define MP1_WM_VF8      0x7BED30u   /* slot 8 */
#define MP1_WM_CLONE    0xB9FC90u   /* (part) -> a new GUI node, callee pops its argument (the stock code calls it with ecx = the GUI) */
#define MP1_WM_ADD      0xB9DBB0u   /* thiscall(gui, node), ret 4 */
#define MP1_WM_PLACE    0x7BE8B0u   /* thiscall(gui, slot *), ret 4: project slot +0x10 (vec4) with the map's view / proj (+0x7B0 / +0x7F0), place part slot +4, show by depth */
#define MP1_WM_VIS      0x847C30u   /* thiscall(gui, part, visible), ret 8 */
#define MP1_WM_POS      0x8D30D0u   /* thiscall(world-map model, float out[4]), ret 4: the local player's position in world-map space (0,0,0 = none); the model is
                                       [uSubMenuManager +0x7C] (FUN_007C10D0: mov edi,[ebx+0x7c] ... mov ecx,edi; call 0x8D33D0), its +0x110 the map node */
#define MP1_SUBMENU_OFF 0x3Cu       /* sIDCockpit +0x3C = uSubMenuManager: +0x78 the 3D map icon GUI, +0x7C the world-map model */
typedef struct { uint8_t on; uint8_t pad[3]; uint32_t part; uint32_t pad2[2]; float pos[4]; } mp1_wm_slot;   /* the stock 0x20-byte marker slot */
/* the partner marker is shown when the patch is wanted, the map shows the player's own marker and the partner has a world-map position */
static inline int mp1_wm_show(int want, int player_marker_on, const float pos[3])
{
    return want && player_marker_on && (pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]) > 1e-6f;
}

/* the 3D map marks a ROOM, not a spot (FUN_008D30D0 maps the position through the room, FUN_008D2EE0): two humans in one room get the same marker
 * place (live mm1s3 03:43: own and partner both at world-map (171.5, -2.5, 0)), the partner marker exactly over the player's own. The partner marker is
 * then drawn MP1_WM_SAME_DX layout pixels to the right of it (node +0xA0, after the stock placement). */
#define MP1_WM_SAME_DX 18.0f
static inline int mp1_wm_same_spot(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz < 0.25f;
}

/* ---- split screen: the minimap in a half viewport ----
 * uGUI_Map_vf9 places the map content at +0x40 / +0x44 = the screen's width / height ([0x11E1B9C] +0xB8 / +0xBC, reads at 0x7B7B39 / 0x7B7B62)
 * * 1/1280 / 1/720 * the layout offset +0xED8 / +0xEDC (856, -353 of the virtual 1280x720 layout), and uMapIcon_vf9 / vf11 compute the icons from the
 * same size (0x7B9654 .. 0x7BA0F3): pixel positions of the WHOLE back buffer. In a split half the map lands outside the half (live mm1s3 03:40: screen
 * 1920 x 1080, +0x40 = 1284.0 while the half is 960 wide), while the generic GUI layout puts the frame into the half: both halves showed only the frame
 * (also in the stage 5 split, 2026-11-04). The fix: those 8 reads take a screen object of the mod's (a cell in place of the address 0x11E1B9C) that
 * answers the half's size while the split draws, the real object at any other time. (0x7B6F01 reads another field of that object: not touched.) */
#define MP1_SCREEN_PTR 0x11E1B9Cu
#define MP1_SCREEN_W   0xB8u
#define MP1_SCREEN_H   0xBCu
#define MP1_N_SCREEN_SITES 8
static const uint32_t mp1_screen_site[MP1_N_SCREEN_SITES] = {0x7B7B39u, 0x7B7B62u, 0x7B9654u, 0x7B9847u, 0x7B991Au, 0x7B9A1Bu, 0x7B9BE0u, 0x7BA0F3u};
/* where the absolute address sits in a `mov reg, [abs]`: a1 = mov eax (1 opcode byte), 8b 05/0d/15 = mov eax/ecx/edx (2 bytes); -1 = another instruction */
static inline int mp1_screen_imm_off(const uint8_t *p)
{
    if (p[0] == 0xA1)
        return 1;
    if (p[0] == 0x8B && (p[1] == 0x05 || p[1] == 0x0D || p[1] == 0x15))
        return 2;
    return -1;
}
/* the size a split half answers: side by side = half the width, top / bottom = half the height */
static inline void mp1_half_size(int vertical, int w, int h, int *ow, int *oh)
{
    *ow = vertical ? w / 2 : w;
    *oh = vertical ? h : h / 2;
}
/* uGUI_Map +0x40 (the map content's x in pixels) for a screen width (the stock formula of 0x7B7B39) */
static inline float mp1_map_x(int w, float layout_x) { return (float)w * (1.0f / 1280.0f) * layout_x; }

/* mapptn1_game.c (the game glue, game thread): once from probe_install; the console command `mapptn status|apply|revert|auto|log` */
int mapptn_init(void);
void mapptn_cmd(const char *arg, char *out, int size);

#endif
