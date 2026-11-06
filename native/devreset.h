/*
 * devreset.h - recovery of a lost Direct3D 9 device (pure logic, no Direct3D or Windows headers: testable offline).
 *
 * The game calls IDirect3DDevice9::Reset once. When the device is lost (D3DERR_DEVICELOST) it shows its own
 * "Fatal error" box at once and ends the run (seen 2026-11-06 04:30 on the Intel adapter while another program
 * started on the other GPU). Direct3D 9 expects the application to wait and try again: TestCooperativeLevel
 * reports D3DERR_DEVICENOTRESET when the device may be reset, and then Reset is called again.
 *
 * devreset_wait() does exactly that, with a time budget, and returns the final Reset result. The Direct3D calls
 * and the clock are passed in as callbacks, so tests can drive every sequence (d3d.c wires the real ones).
 */
#ifndef DEVRESET_H
#define DEVRESET_H

#define DEVRESET_OK        0L
#define DEVRESET_LOST      ((long)0x88760868)  /* D3DERR_DEVICELOST */
#define DEVRESET_NOTRESET  ((long)0x88760869)  /* D3DERR_DEVICENOTRESET */

typedef long (*devreset_call_t)(void *ctx);            /* returns an HRESULT */
typedef void (*devreset_sleep_t)(void *ctx, unsigned ms);
typedef unsigned (*devreset_now_t)(void *ctx);          /* milliseconds, wraps like GetTickCount */

static inline int devreset_is_lost(long hr)
{
    return hr == DEVRESET_LOST || hr == DEVRESET_NOTRESET;
}

/*
 * hr_reset: the result of the first Reset call.
 * coop:     TestCooperativeLevel of the same device.
 * reset:    Reset of the same device with the same parameters.
 * Returns the first result that is not "lost", or the last "lost" result when budget_ms has run out.
 * *tries (may be NULL) counts the Reset calls made here (not the first one).
 */
static inline long devreset_wait(long hr_reset, devreset_call_t coop, devreset_call_t reset,
                                 devreset_sleep_t sleep_fn, devreset_now_t now_fn, void *ctx,
                                 unsigned budget_ms, unsigned poll_ms, unsigned *tries)
{
    unsigned t0;
    long hr = hr_reset;
    if (tries)
        *tries = 0;
    if (!devreset_is_lost(hr))
        return hr;
    t0 = now_fn(ctx);
    for (;;) {
        long c;
        sleep_fn(ctx, poll_ms);
        c = coop(ctx);
        if (c == DEVRESET_OK || c == DEVRESET_NOTRESET) {
            hr = reset(ctx);
            if (tries)
                ++*tries;
            if (!devreset_is_lost(hr))
                return hr;
        }
        if ((unsigned)(now_fn(ctx) - t0) >= budget_ms)
            return hr;
    }
}

#endif
