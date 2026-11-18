/*
 * aimray1.h - fix023: the aim ray of the SECOND human of the split screen (player report 2026-11-17: "when they shoot at zombies, the shots miss and don't damage
 * the enemy"). Pure part (no Windows): the decision, the stub bytes, the site checks; the glue is in split.c, tests/test_aimray1.c checks the decision and the stub layout.
 *
 * FACTS (Ghidra work\ghidra-fix023, <lab>\re1q\f23\o5..o7.txt, the image; notes\p2-shots-023.md):
 *   - a gun's aim / shot ray is made by FUN_005EFE50 (fastcall, ecx = the weapon; called from the upper-body Aim / Fire states, uLaserSight_vf39, the scope camera).
 *     For a weapon whose owner ([weapon+0x13C8], else [[weapon+0x12D0]+0x30]) is a player class with kind (+0xE30) 1 = a HUMAN, the shot goes from +0x12F0 to +0x1370:
 *     the CAMERA ray through the middle of the view, made by FUN_005EE350 (and FUN_005EFAC0's screen point); an AI / remote owner shoots along the muzzle ray instead.
 *   - FUN_005EE350 takes the view's half size from viewport 0's rect ([sCamera]+0x48..+0x54) and FUN_005EBC10 builds the view / projection from
 *     [[sCamera]+0xCE0] = camera manager 0 (eye +0x90, target +0xB0, up +0xA0, fov +0x8C, near / far +0x34 / +0x30, aspect +0x38) and the same rect.
 *     So EVERY human's shots follow camera manager 0 = the FIRST human's view. In a split screen the second human (his own view = manager 1 on viewport 2)
 *     fired along the first human's line of sight, from near the first human's camera: his shots went where the first human looked (0.1.0 .. 0.2.2).
 *   - the three loads of the sCamera pointer in these functions are `mov eax,[0x11E00B8]` (A1 B8 00 1E 01) at 0x5EBC16 (ecx = the weapon: FUN_005EE350 calls with
 *     `mov ecx, esi`), 0x5EE3BB and 0x5EFB1F (esi = the weapon); through eax they read only +0x48, +0x4C, +0x50, +0x54 and +0xCE0 (the disassembly up to the next
 *     write of eax). FUN_005EBC10's only caller is FUN_005EE350.
 * THE FIX: each of the three loads becomes `call stub`; the stub (flags, ecx, edx, xmm0..7 kept) asks ar1_choose(weapon): the real sCamera, or - for a human weapon
 * owner who is NOT the own slot while the split screen shows manager 1 on viewport 2 - a stand-in object whose +0xCE0 is manager 1 and whose rect is viewport 2's
 * size (origin 0, 0: the functions use only the sizes, and FUN_005EFAC0's screen point stays the first human's one). The first human, the AI partner, a
 * network game (no split screen) and the whole view (cutscene, menu) all get the real object: the stock bytes' result.
 */
#ifndef AIMRAY1_H
#define AIMRAY1_H

#include <stdint.h>

#define AR1_CAM_CELL 0x11E00B8u   /* [..] = sGameCamera */
#define AR1_N_SITES 3
#define AR1_SITE_LEN 5
#define AR1_STUB_LEN 112          /* 106 bytes of code + int3 padding */
#define AR1_FAKE_SIZE 0xD10u      /* sGameCamera's size (engine-map): the stand-in is as large, zero apart from the fields below */
#define AR1_OFF_RECT 0x48u        /* sCamera + this: viewport 0's rect (left, top, right, bottom: 4 ints); viewport k's rect = +0x30 + 0x190 k + 0x18 */
#define AR1_OFF_MGR0 0xCE0u
#define AR1_OFF_MGR1 0xCE4u
#define AR1_W_OWNER 0x13C8u       /* weapon + this: its owner unit (0 = ask the holder) */
#define AR1_W_HOLDER 0x12D0u      /* weapon + this: the holder object, its +0x30 = the owner */
#define AR1_HOLDER_OWNER 0x30u
#define AR1_U_KIND 0xE30u         /* unit + this: 1 human, 2 AI, 3 remote */
#define AR1_U_SLOT 0xE2Cu         /* unit + this: the unit slot */

enum { AR1_REG_ECX = 0, AR1_REG_ESI = 1 };
enum { AR1_OFF = 0, AR1_ON, AR1_MISMATCH, AR1_ERROR };

typedef struct {
    uint32_t addr;                /* the 5 bytes `mov eax,[0x11E00B8]` */
    int reg;                      /* where the weapon is at that instruction */
    uint8_t next[8];              /* the bytes right after the instruction (the build check) */
    uint32_t reg_addr;            /* the instruction that put the weapon into that register (the build check) */
    uint8_t reg_bytes[7];
    int reg_len;
    const char *what;
} ar1_site;

extern const ar1_site ar1_sites[AR1_N_SITES];
extern const uint8_t ar1_site_orig[AR1_SITE_LEN];   /* A1 B8 00 1E 01 */

typedef struct {
    int (*read)(void *user, uint32_t addr, void *out, uint32_t n);
    int (*write)(void *user, uint32_t addr, const void *p, uint32_t n);   /* code bytes: the glue lifts the page protection */
    uint32_t (*alloc)(void *user, uint32_t n);                              /* executable memory for the whole process */
    void (*log)(void *user, const char *line);
    void *user;
} ar1_mem;

/* what the split pass saw last (written on the game thread every pass, read by any thread in ar1_choose) */
typedef struct {
    volatile uint32_t on;         /* 1 = gameplay view of an active split: viewport 2 shows manager 1 */
    volatile uint32_t mgr1;       /* [sCamera+0xCE4] of that pass */
    volatile uint32_t own;        /* the own slot of that pass (0 / 1) */
    volatile int32_t w, h;        /* viewport 2's size */
} ar1_snap;

typedef struct {
    uint32_t cam_cell;            /* AR1_CAM_CELL (tests: their own cell) */
    volatile int enabled;         /* 1 = the second human's ray follows his view (default); 0 = the stock behaviour (lab A / B: `split aim 0`, REV1_SPLIT_AIM=0) */
    ar1_snap snap;
    uint32_t fake[AR1_FAKE_SIZE / 4];
    volatile uint32_t n_calls, n_second;   /* counters (plain increments from several threads: approximate) */
    volatile uint32_t last_weapon, last_owner;
    int state;                    /* AR1_OFF / ON / MISMATCH / ERROR */
    uint32_t stub[AR1_N_SITES];
    char why[96];
    unsigned applies, reverts, refusals;
} ar1_t;

extern ar1_t ar1_g;

/* the decision (cdecl, called from the stubs on any thread): the sCamera pointer the stock instruction would have loaded, or the stand-in */
uint32_t ar1_choose(uint32_t weapon);
/* the pure decision for tests: owner fields as read; returns 1 = the second view */
int ar1_second_view(const ar1_snap *s, int enabled, uint32_t real, uint32_t real_mgr1, uint32_t owner, uint32_t kind, uint32_t slot);

void ar1_init(ar1_t *g, uint32_t cam_cell, int enabled);
void ar1_set_view(ar1_t *g, int on, uint32_t mgr1, int own, int w, int h);
int ar1_stub_build(uint8_t *out, uint32_t stub, int reg, uint32_t fn);   /* AR1_STUB_LEN bytes */
void ar1_site_build(uint8_t out[AR1_SITE_LEN], uint32_t site, uint32_t stub);
int ar1_apply(ar1_t *g, const ar1_mem *m, uint32_t fn);                 /* all three sites or none; 1 = on */
int ar1_revert(ar1_t *g, const ar1_mem *m);
const char *ar1_state_name(int st);

#endif
