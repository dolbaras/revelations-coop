/* test_devreset.c: the device-lost wait around IDirect3DDevice9::Reset (native/devreset.h), driven by scripted fakes and a fake clock. */
#include <stdio.h>
#include "../native/devreset.h"
static int bad, n;
static void chk(int ok, const char *what) { n++; if (!ok) { bad++; printf("FAIL: %s\n", what); } }

/* scripted device: coop_seq[i] is what TestCooperativeLevel returns on its i-th call (the last value repeats),
   reset_seq[i] what the i-th extra Reset returns (the last value repeats); the clock moves 100 ms per sleep */
struct fake {
    const long *coop_seq; int coop_n, coop_i;
    const long *reset_seq; int reset_n, reset_i;
    unsigned now;
    int sleeps;
};
static void f_sleep(void *c, unsigned ms) { struct fake *f = c; f->now += ms; f->sleeps++; }
static unsigned f_now(void *c) { return ((struct fake *)c)->now; }
static long f_coop(void *c)
{
    struct fake *f = c; int i = f->coop_i < f->coop_n ? f->coop_i : f->coop_n - 1; f->coop_i++;
    return f->coop_seq[i];
}
static long f_reset(void *c)
{
    struct fake *f = c; int i = f->reset_i < f->reset_n ? f->reset_i : f->reset_n - 1; f->reset_i++;
    return f->reset_seq[i];
}

#define RUN(f, first, budget, tries_out) devreset_wait((first), f_coop, f_reset, f_sleep, f_now, &(f), (budget), 100, (tries_out))

int main(void)
{
    unsigned tries = 99;
    long hr;
    struct fake f;

    /* 1. a successful first Reset passes straight through, nothing is waited for */
    f = (struct fake){0};
    hr = RUN(f, DEVRESET_OK, 20000, &tries);
    chk(hr == DEVRESET_OK && tries == 0 && f.sleeps == 0, "first Reset OK: passed through, no wait");

    /* 2. any other error passes through unchanged (only the lost codes are waited for) */
    f = (struct fake){0};
    hr = RUN(f, (long)0x80004005, 20000, &tries);
    chk(hr == (long)0x80004005 && tries == 0 && f.sleeps == 0, "other error: passed through");

    /* 3. lost, the device is lost for three more checks, then may be reset; the first reset is still lost */
    {
        static const long coop[] = {DEVRESET_LOST, DEVRESET_LOST, DEVRESET_LOST, DEVRESET_NOTRESET};
        static const long reset[] = {DEVRESET_LOST, DEVRESET_OK};
        f = (struct fake){0}; f.coop_seq = coop; f.coop_n = 4; f.reset_seq = reset; f.reset_n = 2;
        hr = RUN(f, DEVRESET_LOST, 20000, &tries);
        chk(hr == DEVRESET_OK, "recovers after the device may be reset");
        chk(tries == 2, "two Reset calls made in the wait (one lost, one ok)");
        chk(f.sleeps == 5, "five 100 ms waits: three lost checks, the not-reset check, the ok check");
    }

    /* 4. the device is ready at once (TestCooperativeLevel OK) and Reset succeeds */
    {
        static const long coop[] = {DEVRESET_OK};
        static const long reset[] = {DEVRESET_OK};
        f = (struct fake){0}; f.coop_seq = coop; f.coop_n = 1; f.reset_seq = reset; f.reset_n = 1;
        hr = RUN(f, DEVRESET_LOST, 20000, &tries);
        chk(hr == DEVRESET_OK && tries == 1 && f.sleeps == 1, "ready at once: one wait, one Reset");
    }

    /* 5. the device never recovers: the last lost code comes back after the budget (1 s = 10 polls) */
    {
        static const long coop[] = {DEVRESET_LOST};
        static const long reset[] = {DEVRESET_LOST};
        f = (struct fake){0}; f.coop_seq = coop; f.coop_n = 1; f.reset_seq = reset; f.reset_n = 1;
        hr = RUN(f, DEVRESET_LOST, 1000, &tries);
        chk(hr == DEVRESET_LOST, "never recovers: lost code returned");
        chk(f.sleeps >= 10 && f.sleeps <= 11, "gave up after about the 1 s budget");
        chk(tries == 0, "no Reset call while the device stays lost");
    }

    /* 6. Reset comes back with a real error after the device was ready: that error is returned at once */
    {
        static const long coop[] = {DEVRESET_NOTRESET};
        static const long reset[] = {(long)0x8876086C};   /* D3DERR_INVALIDCALL */
        f = (struct fake){0}; f.coop_seq = coop; f.coop_n = 1; f.reset_seq = reset; f.reset_n = 1;
        hr = RUN(f, DEVRESET_LOST, 20000, &tries);
        chk(hr == (long)0x8876086C && tries == 1, "a real error from Reset is returned, not retried");
    }

    /* 7. NOTRESET as the first answer of Reset is treated like a lost device */
    {
        static const long coop[] = {DEVRESET_NOTRESET};
        static const long reset[] = {DEVRESET_OK};
        f = (struct fake){0}; f.coop_seq = coop; f.coop_n = 1; f.reset_seq = reset; f.reset_n = 1;
        hr = RUN(f, DEVRESET_NOTRESET, 20000, &tries);
        chk(hr == DEVRESET_OK && tries == 1, "NOTRESET as first answer: waited for and reset");
    }

    /* 8. the device stays lost for a while and comes back well inside the budget (8 polls, budget 20 s) */
    {
        static const long coop[] = {DEVRESET_LOST, DEVRESET_LOST, DEVRESET_LOST, DEVRESET_LOST, DEVRESET_LOST,
                                    DEVRESET_LOST, DEVRESET_LOST, DEVRESET_NOTRESET};
        static const long reset[] = {DEVRESET_OK};
        f = (struct fake){0}; f.coop_seq = coop; f.coop_n = 8; f.reset_seq = reset; f.reset_n = 1;
        hr = RUN(f, DEVRESET_LOST, 20000, &tries);
        chk(hr == DEVRESET_OK && tries == 1 && f.sleeps == 8, "eight lost checks then reset: recovers");
    }

    /* 9. NULL tries pointer is allowed */
    f = (struct fake){0};
    hr = devreset_wait(DEVRESET_OK, f_coop, f_reset, f_sleep, f_now, &f, 1000, 100, NULL);
    chk(hr == DEVRESET_OK, "NULL tries accepted");

    printf("%d checks, %d failed\n", n, bad);
    return bad != 0;
}
