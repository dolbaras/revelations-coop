/*
 * guards1.h - two protective patches of the Revelations 1 network game (build 13298487): the pure part (byte checks, patch bytes, state, status text),
 * independent of Windows. Memory is reached through the g1_mem table: the game glue (guards1_game.c) points it at the process, the offline test
 * (tests/test_guards1.c) at a buffer.
 *
 *  EXIT  the crash exe+1396: the kill record asserts (sRankManagerResult.cpp:0x97F "pItem is invalid pointer") when the human of the own slot has no
 *        knife (item slot 10) and the hit carries the flag 0x80. The bytes `85 ff 74 08 81 ff cd cd cd cd 75 18` at 0x89EF05, byte 0x89EF08 08 -> 20:
 *        the JZ jumps to 0x89EF29 (`test edi,edi / jz`) instead of the assert. (notes\net-world-enemies-re.md section 3)
 *  PICK  the pickup condition FUN_00881DF0(slot) = "slot == 0" in an offline game: the second human (own slot 1) could never take an item. The first
 *        5 bytes become `jmp stub`; the stub answers `slot == [cell]` and returns with `ret 4`; the cell holds the own slot (updated every frame).
 *        (notes\net-items-doors-re.md section 3)
 * Nothing is written when the bytes are not exactly the expected ones; the reason is logged and shown by `guards status`.
 */
#ifndef GUARDS1_H
#define GUARDS1_H
#include <stddef.h>
#include <stdint.h>

#define G1_EXIT_ADDR     0x89EF05u    /* 12 bytes checked */
#define G1_EXIT_PATCH    0x89EF08u    /* 1 byte written */
#define G1_EXIT_TARGET   0x89EF29u    /* where the patched JZ lands: `85 ff 74` (test edi,edi / jz) */
#define G1_PICK_ADDR     0x881DF0u    /* 9 bytes checked, 5 written */
#define G1_PICK_CHECK    9

enum { G1_EXIT = 0, G1_PICK = 1, G1_N = 2 };
enum {
    G1_OFF = 0,        /* original bytes in place */
    G1_ON,             /* our patch in place */
    G1_MISMATCH,       /* the bytes are neither the original nor our patch: nothing written */
    G1_ERROR           /* memory access failed */
};
enum { G1_MODE_AUTO = 0, G1_MODE_ON, G1_MODE_OFF };   /* auto = on while a network session is active (or REV1_GUARDS=1), `guards apply` = on, `guards revert` = off */

typedef struct {
    int (*read)(void *user, uint32_t addr, void *out, uint32_t n);          /* 1 = all bytes read */
    int (*write)(void *user, uint32_t addr, const void *p, uint32_t n);     /* 1 = all bytes written (the glue lifts the page protection) */
    uint32_t (*alloc)(void *user, uint32_t n);                              /* an executable, writable block that lives for the whole process; 0 = failed */
    void (*log)(void *user, const char *line);                              /* may be NULL */
    void *user;
} g1_mem;

typedef struct {
    int mode;                      /* G1_MODE_* chosen by the console */
    int env_on;                    /* REV1_GUARDS=1 */
    uint32_t stub, cell;           /* the pickup stub and the cell it compares with (0 = not allocated yet) */
    uint32_t own;                  /* the own slot as last written into the cell */
    int state[G1_N];               /* G1_OFF / ON / MISMATCH / ERROR as the last attempt left it */
    char why[G1_N][96];            /* the reason of the last refusal */
    unsigned applies[G1_N], reverts[G1_N], refusals[G1_N];
} g1_t;

/* expected bytes (tests use them to build fake memory) */
extern const uint8_t g1_exit_orig[12];
extern const uint8_t g1_pick_orig[G1_PICK_CHECK];
extern const uint8_t g1_exit_target_bytes[3];

/* pure helpers */
int g1_stub_build(uint8_t *out, uint32_t cell_addr);                       /* the stub code, returns its length (19) */
void g1_jmp_build(uint8_t out[5], uint32_t from, uint32_t to);             /* E9 rel32 placed at `from` */
int g1_want(const g1_t *g, int net_active);                                /* 1 = the guards should be on now: ON mode, else (AUTO) REV1_GUARDS=1 or an active network session; OFF mode never */

void g1_init(g1_t *g, int env_on);
/* which = G1_EXIT / G1_PICK. apply: 1 = now on (already on counts), 0 = refused (g->why says why). Both are safe to call every frame. */
int g1_apply(g1_t *g, const g1_mem *m, int which, uint32_t own);
int g1_revert(g1_t *g, const g1_mem *m, int which);
/* the cell follows the own slot while the pickup patch is on (cheap, compares with the last value) */
void g1_set_own(g1_t *g, const g1_mem *m, uint32_t own);
/* reads the live bytes and says what they are: "original" / "patched" / "FOREIGN xx xx ..." / "unreadable" */
int g1_probe(const g1_t *g, const g1_mem *m, int which, char *out, size_t size);
void g1_status(const g1_t *g, const g1_mem *m, int net_active, uint32_t own, char *out, size_t size);
const char *g1_name(int which);
const char *g1_state_name(int st);

/* guards1_game.c (the game glue, game thread): once from probe_install; the console command `guards status|apply|revert|auto` */
int guards_init(void);
void guards_cmd(const char *arg, char *out, int size);

#endif
