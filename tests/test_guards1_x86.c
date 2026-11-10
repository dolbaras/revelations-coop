/* The pickup stub of native/guards1.c EXECUTED as 32-bit code (the game is a 32-bit process): thiscall with one stack argument, the callee pops it, eax = slot == [cell], ecx untouched.
 * Also a jump placed over a function with the same prologue and a call through it. Build: sh tests/build.sh (target x86-windows-gnu). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "../native/guards1.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } else printf("ok   %s\n", msg); } while (0)

typedef unsigned (__attribute__((thiscall)) *own_fn)(void *self, unsigned slot);

/* esp balance and ecx are observed from the inside of a caller written in assembly */
static unsigned call_checked(void *fn, unsigned ecx_val, unsigned slot, unsigned *esp_delta, unsigned *ecx_after)
{
    unsigned r, d, c;
    __asm__ volatile("mov %%esp, %%edi\n\t"
                     "pushl %[slot]\n\t"
                     "mov %[ecx], %%ecx\n\t"
                     "call *%[fn]\n\t"
                     "mov %%esp, %%edx\n\t"
                     "sub %%edi, %%edx\n\t"       /* edx = esp now - esp before the push: 0 when the callee popped the argument */
                     "mov %%ecx, %%esi\n\t"
                     "mov %%edi, %%esp\n\t"
                     : "=a"(r), "=d"(d), "=S"(c)
                     : [slot] "r"(slot), [ecx] "r"(ecx_val), [fn] "r"(fn)
                     : "ecx", "edi", "memory", "cc");
    *esp_delta = d, *ecx_after = c;
    return r;
}

int main(void)
{
    uint8_t *blk = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint32_t *cell = (uint32_t *)(blk + 32);
    CHECK(blk != NULL, "executable block");
    int n = g1_stub_build(blk, (uint32_t)(uintptr_t)cell);
    CHECK(n == 19, "stub length 19");
    own_fn f = (own_fn)(void *)blk;
    unsigned dl, ecx;
    for (unsigned own = 0; own < 2; own++) {
        *cell = own;
        int ok = 1;
        for (unsigned slot = 0; slot < 4; slot++) {
            unsigned r = call_checked(blk, 0x12345678, slot, &dl, &ecx);
            if (r != (slot == own ? 1u : 0u) || dl != 0 || ecx != 0x12345678) ok = 0;
        }
        CHECK(ok, own ? "stub, own slot 1: true only for slot 1, argument popped, ecx kept" : "stub, own slot 0: true only for slot 0 (the stock answer), argument popped, ecx kept");
    }
    *cell = 1;
    CHECK(f((void *)1, 1) == 1 && f((void *)1, 0) == 0, "stub through a C thiscall pointer");

    /* a function standing where 0x881DF0 stands: the same 9 first bytes, then `ret 4` (never reached after the jump) */
    uint8_t *fn = blk + 48;
    memcpy(fn, g1_pick_orig, 9);
    fn[9] = 0xC2, fn[10] = 0x04, fn[11] = 0x00;
    uint8_t jmp[5];
    g1_jmp_build(jmp, (uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)blk);
    memcpy(fn, jmp, 5);
    *cell = 1;
    CHECK(call_checked(fn, 7, 1, &dl, &ecx) == 1 && dl == 0, "a function patched with `jmp stub` answers slot == own slot (1)");
    CHECK(call_checked(fn, 7, 0, &dl, &ecx) == 0 && dl == 0, "... and false for slot 0");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
