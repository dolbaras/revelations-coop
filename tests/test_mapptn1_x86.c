/* The minimap partner stub of native/mapptn1.c EXECUTED as 32-bit code (the game is a 32-bit process). The stub is built with its targets pointed at
 * test code in the same executable block: a harness enters it the way the game does (a fake frame of uGUI_Map_vf9 with the partner slot [esp+0x14],
 * edx = the game-systems holder, ebx = 0), through the patched 9-byte site; three exits record which stock path was taken, the partner slot and ESP; a fake
 * FUN_0041BC10 (thiscall, one stack argument, ret 4) records ecx and its argument. Build: sh tests/build.sh (target x86-windows-gnu). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "../native/mapptn1.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } else printf("ok   %s\n", msg); } while (0)

enum { O_STUB = 0x000, O_SITE = 0x040, O_ENTRY = 0x060, O_RAID = 0x0A0, O_NONE = 0x0D0, O_CONT = 0x100, O_FN = 0x180, O_DATA = 0x200 };
enum { D_TAG = 0, D_ESP0 = 4, D_ESPC = 8, D_ECX = 12, D_ARG = 16, D_PARTNER = 20, D_SPLAYER = 24, D_CALLS = 28 };

static uint8_t *blk;
static uint32_t A(int off) { return (uint32_t)(uintptr_t)(blk + off); }
static uint32_t *cell(int d) { return (uint32_t *)(blk + O_DATA + d); }
static void p32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* exit snippet: mov [espc],esp; mov dword [tag],K; mov eax,[esp+0x14]; lea esp,[ebp-0xc]; pop edi; pop esi; pop ebx; pop ebp; ret */
static void exit_snippet(int off, uint32_t tag)
{
    uint8_t *p = blk + off;
    p[0] = 0x89, p[1] = 0x25, p32(p + 2, A(O_DATA + D_ESPC));
    p[6] = 0xc7, p[7] = 0x05, p32(p + 8, A(O_DATA + D_TAG)), p32(p + 12, tag);
    memcpy(p + 16, "\x8b\x44\x24\x14\x8d\x65\xf4\x5f\x5e\x5b\x5d\xc3", 12);
}

static void build(void)
{
    memset(blk, 0xCC, 4096);
    mp1_targets t = {A(O_RAID), A(O_NONE), A(O_CONT), A(O_FN), A(O_DATA + D_SPLAYER)};
    mp1_stub_build(blk + O_STUB, A(O_STUB), &t);
    mp1_site_build_at(blk + O_SITE, A(O_SITE), A(O_STUB));
    /* entry (cdecl, one argument = the holder): push ebp; mov ebp,esp; push ebx; push esi; push edi; sub esp,0x40; mov dword [esp+0x14],0x5a5a5a5a;
     * xor ebx,ebx; mov edx,[ebp+8]; mov [esp0],esp; jmp site */
    uint8_t *e = blk + O_ENTRY;
    memcpy(e, "\x55\x89\xe5\x53\x56\x57\x83\xec\x40\xc7\x44\x24\x14\x5a\x5a\x5a\x5a\x31\xdb\x8b\x55\x08", 22);
    e[22] = 0x89, e[23] = 0x25, p32(e + 24, A(O_DATA + D_ESP0));
    e[28] = 0xe9, p32(e + 29, A(O_SITE) - (A(O_ENTRY) + 33));
    exit_snippet(O_RAID, 1);
    exit_snippet(O_NONE, 2);
    exit_snippet(O_CONT, 3);
    /* fake FUN_0041BC10: inc dword [calls]; mov [ecxcell],ecx; mov eax,[esp+4]; mov [argcell],eax; mov eax,[partner]; ret 4 */
    uint8_t *f = blk + O_FN;
    f[0] = 0xff, f[1] = 0x05, p32(f + 2, A(O_DATA + D_CALLS));
    f[6] = 0x89, f[7] = 0x0d, p32(f + 8, A(O_DATA + D_ECX));
    memcpy(f + 12, "\x8b\x44\x24\x04", 4);
    f[16] = 0xa3, p32(f + 17, A(O_DATA + D_ARG));
    f[21] = 0xa1, p32(f + 22, A(O_DATA + D_PARTNER));
    f[26] = 0xc2, f[27] = 0x04, f[28] = 0x00;
}

typedef uint32_t (__attribute__((cdecl)) *entry_fn)(void *holder);

/* the game-systems holder: +0x24 -> an object whose +0x20 is the game mode (1 = Raid) */
static struct { uint8_t pad[0x24]; void *mode_obj; } holder;
static struct { uint8_t pad[0x20]; uint32_t mode; } mode_obj;

static uint32_t run(uint32_t mode, uint32_t splayer, uint32_t partner)
{
    mode_obj.mode = mode;
    holder.mode_obj = &mode_obj;
    *cell(D_TAG) = 0, *cell(D_ESP0) = 0, *cell(D_ESPC) = 0, *cell(D_ECX) = 0, *cell(D_ARG) = 0, *cell(D_CALLS) = 0;
    *cell(D_SPLAYER) = splayer, *cell(D_PARTNER) = partner;
    return ((entry_fn)(void *)(blk + O_ENTRY))(&holder);
}

int main(void)
{
    blk = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(blk != NULL, "executable block");
    build();

    uint32_t r = run(1, 0x13579BDF, 0x2468ACE0);
    CHECK(*cell(D_TAG) == 1 && *cell(D_CALLS) == 0 && r == 0x5a5a5a5a, "Raid (mode 1): the stock partner block, the stub calls nothing");
    CHECK(*cell(D_ESPC) == *cell(D_ESP0), "... ESP unchanged");

    r = run(0, 0, 0x2468ACE0);
    CHECK(*cell(D_TAG) == 2 && *cell(D_CALLS) == 0, "campaign, no sPlayer: the stock no-partner store, nothing called");

    r = run(0, 0x13579BDF, 0x2468ACE0);
    CHECK(*cell(D_TAG) == 3, "campaign co-op: continues at the stock partner placement (0x7B7F2C)");
    CHECK(r == 0x2468ACE0, "... with the partner of FUN_0041BC10 in vf9's slot [esp+0x14]");
    CHECK(*cell(D_CALLS) == 1 && *cell(D_ECX) == 0x13579BDF, "... FUN_0041BC10 called once as thiscall on sPlayer");
    CHECK(*cell(D_ARG) == *cell(D_ESP0) + 0x13, "... with &[esp+0x13] as its argument (the stock call's)");
    CHECK(*cell(D_ESPC) == *cell(D_ESP0), "... ESP balanced (the callee popped its argument)");

    r = run(0, 0x13579BDF, 0);
    CHECK(*cell(D_TAG) == 3 && r == 0, "campaign co-op without a partner unit: partner slot 0 (the stock code then hides the icon)");

    r = run(2, 0x13579BDF, 0x11112222);
    CHECK(*cell(D_TAG) == 3 && r == 0x11112222, "any non-Raid mode counts as campaign");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
