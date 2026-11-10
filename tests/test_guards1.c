/* Offline test of the network guards (native/guards1.c): byte checks before every write, the exact bytes written, revert, refusal reasons, the pickup stub, the switch rules.
 * The fake memory holds the REAL bytes of the supported build around both patch points (read from rerev.exe, build 13298487). Build + run: sh tests/build.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/guards1.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } else printf("ok   %s\n", msg); } while (0)

/* ---- fake memory: three regions ---- */
#define R_EXIT 0x89EF00u
#define R_PICK 0x881DE0u
#define R_HEAP 0x20000000u
static uint8_t m_exit[0x40], m_pick[0x40], m_heap[128];
static const uint8_t real_exit[0x30] = {0x0a, 0xff, 0xd0, 0x8b, 0xf8, 0x85, 0xff, 0x74, 0x08, 0x81, 0xff, 0xcd, 0xcd, 0xcd, 0xcd, 0x75, 0x18, 0x57, 0x68, 0xf8, 0x56, 0x09, 0x01, 0x68,
                                        0x7f, 0x09, 0x00, 0x00, 0x68, 0xb0, 0x55, 0x09, 0x01, 0xe8, 0x4a, 0x24, 0xb6, 0xff, 0x83, 0xc4, 0x10, 0x85, 0xff, 0x74, 0x37, 0x8b, 0xcf, 0xe8};
static const uint8_t real_pick[0x18] = {0x51, 0x56, 0x8b, 0xf1, 0xe8, 0x47, 0x55, 0x14, 0x00, 0x85, 0xc0, 0x74, 0x30, 0x8a, 0x80, 0x88, 0xaa, 0x00, 0x00, 0x24, 0x01, 0x74, 0x26, 0x8b};
static int writes, fail_write, fail_alloc, unreadable;
static char lastlog[240], alllog[4096];

static uint8_t *at(uint32_t a, uint32_t n)
{
    if (a >= R_EXIT && a + n <= R_EXIT + sizeof m_exit) return m_exit + (a - R_EXIT);
    if (a >= R_PICK && a + n <= R_PICK + sizeof m_pick) return m_pick + (a - R_PICK);
    if (a >= R_HEAP && a + n <= R_HEAP + sizeof m_heap) return m_heap + (a - R_HEAP);
    return NULL;
}
static int f_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    uint8_t *p = unreadable ? NULL : at(a, n);
    if (!p) return 0;
    memcpy(out, p, n);
    return 1;
}
static int f_write(void *u, uint32_t a, const void *src, uint32_t n)
{
    (void)u;
    uint8_t *p = at(a, n);
    if (!p || fail_write) return 0;
    memcpy(p, src, n);
    writes++;
    return 1;
}
static uint32_t f_alloc(void *u, uint32_t n) { (void)u, (void)n; return fail_alloc ? 0 : R_HEAP; }
static void f_log(void *u, const char *l)
{
    (void)u;
    snprintf(lastlog, sizeof lastlog, "%s", l);
    strncat(alllog, l, sizeof alllog - strlen(alllog) - 2);
    strcat(alllog, "\n");
}
static const g1_mem MEM = {f_read, f_write, f_alloc, f_log, NULL};

static void reset(void)
{
    memset(m_exit, 0, sizeof m_exit), memset(m_pick, 0, sizeof m_pick), memset(m_heap, 0xCC, sizeof m_heap);
    memcpy(m_exit, real_exit, sizeof real_exit);
    memcpy(m_pick + (0x881DF0 - R_PICK), real_pick, sizeof real_pick);
    writes = fail_write = fail_alloc = unreadable = 0;
    lastlog[0] = alllog[0] = 0;
}
static int diff_bytes(const uint8_t *a, const uint8_t *b, int n)
{
    int d = 0;
    for (int i = 0; i < n; i++) d += a[i] != b[i];
    return d;
}

/* a tiny interpreter of exactly the instructions of the stub: proves the encoding reads [esp+4], compares with the cell, answers 0 / 1 and pops 4 bytes */
static int run_stub(const uint8_t *code, int len, uint32_t cell_addr, uint32_t cell_val, uint32_t arg, int *pops)
{
    int i = 0;
    uint32_t eax = 0xDEAD0000u;
    int zf = 0;
    if (!(len >= 4 && code[0] == 0x8B && code[1] == 0x44 && code[2] == 0x24 && code[3] == 0x04)) return -1;   /* mov eax,[esp+4] */
    eax = arg, i = 4;
    if (!(code[i] == 0x3B && code[i + 1] == 0x05)) return -2;                                                 /* cmp eax,[disp32] */
    uint32_t d;
    memcpy(&d, code + i + 2, 4);
    if (d != cell_addr) return -3;
    zf = eax == cell_val, i += 6;
    if (!(code[i] == 0x0F && code[i + 1] == 0x94 && code[i + 2] == 0xC0)) return -4;                           /* sete al */
    eax = (eax & ~0xFFu) | (uint32_t)zf, i += 3;
    if (!(code[i] == 0x0F && code[i + 1] == 0xB6 && code[i + 2] == 0xC0)) return -5;                           /* movzx eax,al */
    eax &= 0xFF, i += 3;
    if (!(code[i] == 0xC2 && code[i + 1] == 0x04 && code[i + 2] == 0x00)) return -6;                           /* ret 4 */
    *pops = 4, i += 3;
    return i == len ? (int)eax : -7;
}

int main(void)
{
    g1_t g;
    char buf[1024], live[100];
    uint8_t before[0x40];

    /* ---- the switch rules ---- */
    g1_init(&g, 0);
    CHECK(!g1_want(&g, 0), "want: single player / split screen (no session, no flag) = off");
    CHECK(g1_want(&g, 1), "want: an active network session = on");
    g.mode = G1_MODE_OFF;
    CHECK(!g1_want(&g, 1), "want: `guards revert` wins over the session");
    g.mode = G1_MODE_ON;
    CHECK(g1_want(&g, 0), "want: `guards apply` forces on without a session");
    g1_init(&g, 1);
    CHECK(g1_want(&g, 0), "want: REV1_GUARDS=1 = on without a session");

    /* ---- helpers ---- */
    uint8_t jmp[5];
    g1_jmp_build(jmp, 0x881DF0, 0x20000000);
    CHECK(jmp[0] == 0xE9 && *(int32_t *)(jmp + 1) == (int32_t)(0x20000000 - 0x881DF5), "jmp rel32 = target - (from + 5)");
    CHECK(0x89EF09u + 0x20u == G1_EXIT_TARGET, "exit patch: JZ at 0x89EF07 with 0x20 lands on 0x89EF29");

    /* ---- exit guard ---- */
    reset();
    g1_init(&g, 0);
    memcpy(before, m_exit, sizeof before);
    CHECK(g1_probe(&g, &MEM, G1_EXIT, live, sizeof live) == G1_OFF && !strcmp(live, "original"), "exit: real bytes are recognised as original");
    CHECK(g1_apply(&g, &MEM, G1_EXIT, 0) == 1, "exit: apply on the real bytes");
    CHECK(diff_bytes(before, m_exit, sizeof m_exit) == 1 && m_exit[0x08] == 0x20 && before[0x08] == 0x08, "exit: exactly one byte changed, 0x89EF08 08 -> 20");
    CHECK(g.state[G1_EXIT] == G1_ON && g.applies[G1_EXIT] == 1 && writes == 1, "exit: state ON, one write");
    CHECK(strstr(lastlog, "exit patch written") != NULL, "exit: the write is logged");
    CHECK(g1_apply(&g, &MEM, G1_EXIT, 0) == 1 && writes == 1 && g.applies[G1_EXIT] == 1, "exit: a second apply writes nothing");
    CHECK(g1_probe(&g, &MEM, G1_EXIT, live, sizeof live) == G1_ON, "exit: probe says patched");
    CHECK(g1_revert(&g, &MEM, G1_EXIT) == 1 && diff_bytes(before, m_exit, sizeof m_exit) == 0 && g.state[G1_EXIT] == G1_OFF, "exit: revert gives back the original bytes");
    CHECK(g1_revert(&g, &MEM, G1_EXIT) == 1 && g.reverts[G1_EXIT] == 1, "exit: a second revert is harmless");

    /* every one of the 12 expected bytes matters */
    int refused = 0, untouched = 1;
    for (int i = 0; i < 12; i++) {
        reset();
        g1_init(&g, 0);
        m_exit[5 + i] ^= 0x5A;
        memcpy(before, m_exit, sizeof before);
        if (!g1_apply(&g, &MEM, G1_EXIT, 0) && g.state[G1_EXIT] == G1_MISMATCH) refused++;
        if (memcmp(before, m_exit, sizeof before) != 0 || writes) untouched = 0;
    }
    CHECK(refused == 12 && untouched, "exit: a changed byte in any of the 12 positions refuses and writes nothing");
    reset();
    g1_init(&g, 0);
    m_exit[0x29] = 0x90;
    CHECK(!g1_apply(&g, &MEM, G1_EXIT, 0) && !writes && strstr(g.why[G1_EXIT], "jump target"), "exit: the landing place must be `test edi,edi / jz`");
    reset();
    g1_init(&g, 0);
    m_exit[0x08] = 0x30;   /* somebody else's patch in the same byte */
    CHECK(!g1_apply(&g, &MEM, G1_EXIT, 0) && !writes && strstr(g.why[G1_EXIT], "bytes at 0089ef05 are"), "exit: a foreign value is refused with the bytes in the reason");
    CHECK(strstr(lastlog, "not written") != NULL, "exit: the refusal is logged");
    CHECK(g.refusals[G1_EXIT] == 1, "exit: refusal counted once");
    g1_apply(&g, &MEM, G1_EXIT, 0), g1_apply(&g, &MEM, G1_EXIT, 0);
    CHECK(g.refusals[G1_EXIT] == 1, "exit: asking again every frame does not repeat the log / count");
    CHECK(!g1_revert(&g, &MEM, G1_EXIT) && m_exit[0x08] == 0x30, "exit: a revert never touches bytes that are not ours");
    g1_apply(&g, &MEM, G1_EXIT, 0);
    g1_status(&g, &MEM, 1, 1, buf, sizeof buf);
    CHECK(strstr(buf, "FOREIGN") && strstr(buf, "why: bytes at") && strstr(buf, "REFUSED"), "exit: status shows the foreign bytes and the reason");
    reset();
    g1_init(&g, 0);
    fail_write = 1;
    CHECK(!g1_apply(&g, &MEM, G1_EXIT, 0) && g.state[G1_EXIT] == G1_ERROR && m_exit[8] == 0x08, "exit: a failed write is an ERROR, state not ON");
    reset();
    g1_init(&g, 0);
    unreadable = 1;
    CHECK(!g1_apply(&g, &MEM, G1_EXIT, 0) && g.state[G1_EXIT] == G1_ERROR && !writes, "exit: unreadable memory writes nothing");

    /* ---- pickup guard ---- */
    reset();
    g1_init(&g, 0);
    memcpy(before, m_pick, sizeof before);
    uint32_t off = 0x881DF0 - R_PICK;
    CHECK(g1_probe(&g, &MEM, G1_PICK, live, sizeof live) == G1_OFF, "pick: real bytes are recognised as original");
    CHECK(g1_apply(&g, &MEM, G1_PICK, 1) == 1, "pick: apply with own slot 1");
    CHECK(g.stub == R_HEAP && g.cell == R_HEAP + 32 && g.state[G1_PICK] == G1_ON, "pick: stub and cell allocated, state ON");
    CHECK(diff_bytes(before, m_pick, sizeof m_pick) <= 5 && memcmp(m_pick + off + 5, real_pick + 5, 4) == 0, "pick: only the first 5 bytes of the function changed");
    CHECK(m_pick[off] == 0xE9 && *(int32_t *)(m_pick + off + 1) == (int32_t)(g.stub - (0x881DF0 + 5)), "pick: first bytes are `jmp stub`");
    int pops = 0, len = g1_stub_build(m_heap + 0x40, g.cell);   /* build once more in spare heap bytes to compare with what apply wrote */
    CHECK(len == 19 && memcmp(m_heap, m_heap + 0x40, 19) == 0, "pick: the stub in memory is the stub built for that cell");
    uint32_t cellv;
    memcpy(&cellv, m_heap + 32, 4);
    CHECK(cellv == 1, "pick: the cell holds the own slot (1)");
    CHECK(run_stub(m_heap, len, g.cell, cellv, 1, &pops) == 1 && pops == 4, "pick: slot 1 is the owner now (returns 1, ret 4)");
    CHECK(run_stub(m_heap, len, g.cell, cellv, 0, &pops) == 0, "pick: slot 0 is not the owner of a client with own slot 1");
    g1_set_own(&g, &MEM, 0);
    memcpy(&cellv, m_heap + 32, 4);
    CHECK(cellv == 0 && run_stub(m_heap, len, g.cell, cellv, 0, &pops) == 1 && run_stub(m_heap, len, g.cell, cellv, 1, &pops) == 0, "pick: own slot 0 behaves like the stock game (slot == 0)");
    writes = 0;
    g1_set_own(&g, &MEM, 0);
    CHECK(writes == 0, "pick: an unchanged own slot writes nothing");
    CHECK(g1_apply(&g, &MEM, G1_PICK, 1) == 1 && g.applies[G1_PICK] == 1, "pick: a second apply is recognised as ours");
    CHECK(g1_probe(&g, &MEM, G1_PICK, live, sizeof live) == G1_ON, "pick: probe says patched");
    CHECK(g1_revert(&g, &MEM, G1_PICK) == 1 && diff_bytes(before, m_pick, sizeof m_pick) == 0 && g.state[G1_PICK] == G1_OFF, "pick: revert gives back the original bytes");
    g1_set_own(&g, &MEM, 1);
    memcpy(&cellv, m_heap + 32, 4);
    CHECK(cellv == 0, "pick: the cell is left alone while the patch is off");
    CHECK(g1_apply(&g, &MEM, G1_PICK, 1) == 1 && g.stub == R_HEAP && g.applies[G1_PICK] == 2, "pick: re-apply reuses the stub");
    memcpy(&cellv, m_heap + 32, 4);
    CHECK(cellv == 1, "pick: re-apply refreshes the own slot");
    g1_revert(&g, &MEM, G1_PICK);

    for (int i = 0; i < G1_PICK_CHECK; i++) {
        reset();
        g1_init(&g, 0);
        m_pick[off + (uint32_t)i] ^= 0x33;
        memcpy(before, m_pick, sizeof before);
        if (!g1_apply(&g, &MEM, G1_PICK, 1) && g.state[G1_PICK] == G1_MISMATCH && memcmp(before, m_pick, sizeof before) == 0 && !writes && !g.stub)
            continue;
        CHECK(0, "pick: a changed byte must refuse");
    }
    CHECK(1, "pick: a changed byte in any of the 9 positions refuses, writes nothing, allocates nothing");
    reset();
    g1_init(&g, 0);
    m_pick[off] = 0xE9;   /* somebody else's detour */
    CHECK(!g1_apply(&g, &MEM, G1_PICK, 1) && strstr(g.why[G1_PICK], "bytes at 00881df0 are e9"), "pick: a foreign detour is refused with the bytes in the reason");
    CHECK(!g1_revert(&g, &MEM, G1_PICK) && m_pick[off] == 0xE9, "pick: revert leaves a foreign detour alone");
    reset();
    g1_init(&g, 0);
    fail_alloc = 1;
    CHECK(!g1_apply(&g, &MEM, G1_PICK, 1) && g.state[G1_PICK] == G1_ERROR && memcmp(m_pick + off, real_pick, 9) == 0, "pick: no memory for the stub = not patched");
    reset();
    g1_init(&g, 0);
    fail_write = 1;
    CHECK(!g1_apply(&g, &MEM, G1_PICK, 1) && memcmp(m_pick + off, real_pick, 9) == 0, "pick: a failed write leaves the function original");

    /* ---- both together through the status ---- */
    reset();
    g1_init(&g, 0);
    g.mode = G1_MODE_AUTO;
    g1_status(&g, &MEM, 0, 0, buf, sizeof buf);
    CHECK(strstr(buf, "mode auto") && strstr(buf, "network session no") && strstr(buf, "wanted off") && strstr(buf, "live bytes original"), "status: idle game = auto, no session, wanted off, both original");
    g1_apply(&g, &MEM, G1_EXIT, 1), g1_apply(&g, &MEM, G1_PICK, 1);
    g1_status(&g, &MEM, 1, 1, buf, sizeof buf);
    CHECK(strstr(buf, "network session ACTIVE") && strstr(buf, "wanted on") && strstr(buf, "patched (byte 0089ef08 = 20)") && strstr(buf, "patched (jmp 20000000)") && strstr(buf, "in step"),
          "status: session active, both patched, cell in step");
    g1_status(&g, &MEM, 1, 0, buf, sizeof buf);
    CHECK(strstr(buf, "BEHIND") != NULL, "status: a cell that lags behind the own slot is shown");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
