/* test_hunk1.c - Hunk as the second human of the solo part (native/hunk1.h): zig cc -o test_hunk1.exe test_hunk1.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/hunk1.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* the facts the split unlock of world1 (bag1_game.inc hunk_split_unlock) relies on, in the real image (REV1_IMAGE, default ../../image/rerev-image.bin): FsmLockPlayer's exec
 * FUN_0074FBD0 locks / unlocks "the partner" FUN_0041BC10 with FUN_006B3B80 / FUN_006AE390; the lock is E1C |= 4, F44 |= 8; the unlock is thiscall without arguments (plain ret) */
static unsigned char img[0xE20000];
static void t_image(void)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("(no executable image at %s: the unlock facts are not compared)\n", path); return; }
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    CHECK(n > 0x74FC80u - 0x400000u);
    if (n <= 0x74FC80u - 0x400000u) return;
#define AT(va) (img + ((va) - 0x400000u))
    static const unsigned char unl[] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x9E, 0x1C, 0x0E, 0x00, 0x00};
    CHECK(!memcmp(AT(0x6AE390u), unl, sizeof unl));                                                  /* the unlock's prologue (bag1_game.inc checks the same) */
    CHECK(AT(0x6AE495u)[0] == 0xC3 && AT(0x6AE4C8u)[0] == 0xC3 && AT(0x6AE4CDu)[0] == 0xC3);           /* plain ret: no stack argument */
    static const unsigned char lk[] = {0x83, 0x8E, 0x1C, 0x0E, 0x00, 0x00, 0x04, 0x83, 0x8E, 0x44, 0x0F, 0x00, 0x00, 0x08};
    CHECK(!memcmp(AT(0x6B3BBDu), lk, sizeof lk));                                                    /* the lock: or [esi+0xE1C], 4; or [esi+0xF44], 8 */
    int r1, r2, r3;
    memcpy(&r1, AT(0x74FC2Du), 4), memcpy(&r2, AT(0x74FC6Au), 4), memcpy(&r3, AT(0x74FC78u), 4);
    CHECK(AT(0x74FC2Cu)[0] == 0xE8 && 0x74FC31u + (unsigned)r1 == 0x41BC10u);                          /* FsmLockPlayer: the partner */
    CHECK(AT(0x74FC69u)[0] == 0xE8 && 0x74FC6Eu + (unsigned)r2 == 0x6B3B80u);                          /* mIsLockPartner 1: lock */
    CHECK(AT(0x74FC77u)[0] == 0xE8 && 0x74FC7Cu + (unsigned)r3 == 0x6AE390u);                          /* mIsLockPartner 0: unlock */
    printf("ok   image %s: FsmLockPlayer -> partner FUN_0041BC10, lock FUN_006B3B80 (E1C|4, F44|8), unlock FUN_006AE390 (prologue, plain ret)\n", path);
}


int main(void)
{
    const uint32_t A11 = 0x91010100u, A13 = 0x91010300u, A22 = 0x91020200u, A161 = 0x91100100u, A162 = 0x91100200u, ANOT = 0x910E0000u, AEND = 0x91FF0000u;
    hk_t h;

    /* the option is off (REV1_LAB_HUNK=0; the product default is HK_PRODUCT, below): nothing is ever written */
    hk_reset(&h);
    CHECK(hk_step(&h, 0, A11) == HK_KEEP && hk_step(&h, 0, A162) == HK_KEEP && hk_step(&h, 0, A13) == HK_KEEP);
    CHECK(h.sets == 0 && h.clears == 0 && !h.on);
    CHECK(hk_spectate(0, A162) == 1 && hk_spectate(0, A22) == 2 && hk_spectate(0, A161) == 2 && hk_spectate(0, A11) == 0);

    /* on: only the solo part sets the class, paired sections and non-sections never */
    hk_reset(&h);
    CHECK(hk_wanted(1, A162) && !hk_wanted(1, A161) && !hk_wanted(1, A22) && !hk_wanted(1, A11) && !hk_wanted(1, ANOT) && !hk_wanted(1, AEND) && !hk_wanted(1, 0));
    CHECK(!hk_wanted(0, A162));
    CHECK(hk_step(&h, 1, A11) == HK_KEEP);                 /* a paired section before: nothing to do */
    CHECK(hk_step(&h, 1, A161) == HK_KEEP);
    CHECK(hk_step(&h, 1, A162) == HK_SET && h.on && h.area == A162);
    CHECK(hk_step(&h, 1, A162) == HK_SET && h.sets == 2);   /* an in-level restart (death, Continue): written again, still on */
    CHECK(hk_step(&h, 1, A13) == HK_CLEAR && !h.on);       /* the next section takes the id away */
    CHECK(hk_step(&h, 1, A13) == HK_KEEP && h.clears == 1);
    CHECK(hk_step(&h, 1, A162) == HK_SET);                  /* New game / chapter select back into 16-2 */
    CHECK(hk_step(&h, 1, ANOT) == HK_CLEAR);                /* the game leaves the campaign: cleared */
    CHECK(hk_step(&h, 1, A162) == HK_SET && hk_step(&h, 1, AEND) == HK_CLEAR);

    /* the option is switched off while the solo part runs: the next start clears */
    CHECK(hk_step(&h, 1, A162) == HK_SET);
    CHECK(hk_step(&h, 0, A162) == HK_CLEAR && !h.on);

    /* spectator rule: with Hunk the solo part is played by two, 2-2 and 16-1 stay watched, every other area is unchanged */
    CHECK(hk_spectate(1, A162) == 0 && hk_spectate(1, A22) == 2 && hk_spectate(1, A161) == 2 && hk_spectate(1, A11) == 0 && hk_spectate(1, ANOT) == 0);
    CHECK(sec_spectate(A162) == 1);   /* the plain rule itself is untouched */

    /* network roles: host = slot 0 (Parker), guest = slot 1 (Hunk), the same as in a paired section */
    CHECK(hk_own_slot(1) == 0 && hk_own_slot(0) == HK_SLOT);
    CHECK(HK_CLASS_HUNK == 0x800110B0u && (HK_CLASS_HUNK & 0xFFFF0000u) == 0x80010000u);

    /* LAB mode 2: Hunk in every paired section and the solo part, never in a non-section / the end; the spectator rule is the one of mode 1 */
    hk_reset(&h);
    CHECK(hk_wanted(2, A11) && hk_wanted(2, A13) && hk_wanted(2, A22) && hk_wanted(2, A162) && !hk_wanted(2, ANOT) && !hk_wanted(2, AEND) && !hk_wanted(2, 0));
    CHECK(hk_step(&h, 2, A11) == HK_SET && hk_step(&h, 2, A13) == HK_SET && hk_step(&h, 2, A162) == HK_SET && hk_step(&h, 2, ANOT) == HK_CLEAR && hk_step(&h, 2, ANOT) == HK_KEEP);
    CHECK(hk_spectate(2, A162) == 0 && hk_spectate(2, A22) == 2 && hk_spectate(2, A11) == 0);

    /* LAB mode 3: the solo part and the story-locked sections (2-2 / 16-1), not the other paired ones; those sections are no longer watched */
    CHECK(hk_wanted(3, A162) && hk_wanted(3, A22) && hk_wanted(3, A161) && !hk_wanted(3, A11) && !hk_wanted(3, A13) && !hk_wanted(3, ANOT));
    CHECK(hk_spectate(3, A22) == 0 && hk_spectate(3, A161) == 0 && hk_spectate(3, A162) == 0 && hk_spectate(3, A11) == 0);
    hk_reset(&h);
    CHECK(hk_step(&h, 3, A11) == HK_KEEP && hk_step(&h, 3, A22) == HK_SET && hk_step(&h, 3, A13) == HK_CLEAR);

    /* the PRODUCT (HK_PRODUCT, 2026-11-12): 2-2 only; 16-1 and 16-2 (the hidden TGS episode) keep the spectator rule, no paired section, no non-section */
    {
        const uint32_t A21 = 0x91020100u, A31 = 0x91030100u;
        CHECK(HK_PRODUCT != 0 && HK_PRODUCT != 1 && HK_PRODUCT != 2 && HK_PRODUCT != 3);
        CHECK(hk_wanted(HK_PRODUCT, A22) && !hk_wanted(HK_PRODUCT, A161) && !hk_wanted(HK_PRODUCT, A162) && !hk_wanted(HK_PRODUCT, A11) && !hk_wanted(HK_PRODUCT, A13));
        CHECK(!hk_wanted(HK_PRODUCT, A21) && !hk_wanted(HK_PRODUCT, A31) && !hk_wanted(HK_PRODUCT, ANOT) && !hk_wanted(HK_PRODUCT, AEND) && !hk_wanted(HK_PRODUCT, 0));
        CHECK(hk_spectate(HK_PRODUCT, A22) == 0 && hk_spectate(HK_PRODUCT, A161) == 2 && hk_spectate(HK_PRODUCT, A162) == 1 && hk_spectate(HK_PRODUCT, A11) == 0);
        CHECK(hk_ep16(A161) && hk_ep16(A162) && !hk_ep16(A22) && !hk_ep16(A11));
        hk_reset(&h);   /* S7 live: 2-1 -> 2-2 (Hunk) -> 3-1 (the story's pair again) */
        CHECK(hk_step(&h, HK_PRODUCT, A21) == HK_KEEP && hk_step(&h, HK_PRODUCT, A22) == HK_SET && h.on);
        CHECK(hk_step(&h, HK_PRODUCT, A22) == HK_SET);   /* a reload of 2-2 */
        CHECK(hk_step(&h, HK_PRODUCT, A31) == HK_CLEAR && !h.on && hk_step(&h, HK_PRODUCT, A31) == HK_KEEP);
        CHECK(hk_step(&h, HK_PRODUCT, A161) == HK_KEEP && hk_step(&h, HK_PRODUCT, A162) == HK_KEEP);
    }

    t_image();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
