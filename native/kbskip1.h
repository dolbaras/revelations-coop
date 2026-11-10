/*
 * kbskip1.h - the narrow keyboard-mode fix of Revelations 1 (pure: no Windows, no game; header only, used by capture.c and tests/test_kbskip1.c).
 *
 * Static facts (build 13298487; REA decompile work\rea\decomp_88CB10.json, capstone on work\image\rerev-image.bin, Ghidra raw <lab>\re1q\p\o6/o9/o12.txt;
 * notes\net-puppet-impl.md sections 17, 22, 23):
 *   - sGamePad ([0x11E0388]) holds two Pad objects at +0x3C + 0x2CC*k. The RAW block of pad k is sGamePad+0x198 + 0x2CC*k (= Pad+0x15C), 0x2C bytes, written by the XInput poll
 *     FUN_009BDE20: +0x00 held (buttons AND the stick-direction bits 0x10000.. of a stick past the dead zone), +0x04 previous held, +0x08 pressed, +0x0C released, +0x10 changed,
 *     +0x14 0, +0x18 RX, +0x1C RY, +0x20 LX, +0x24 LY, +0x28 RT byte, +0x29 LT byte. The next poll takes "previous held" from +0x00 of the block itself.
 *   - sGamePad_vf6 (0x88E270, the ONLY caller, call at 0x88E45A, return value unused) per frame: poll of every Pad, FUN_00888FD0 (keyboard mode +0x874: set by keyboard / mouse
 *     activity, cleared in a frame in which the CURRENT slot's raw held is non-zero), FUN_00888E70, FUN_0088CB10, then the PadData builder FUN_0088A470 for k = 0, 1.
 *   - FUN_0088CB10 (ecx = sGamePad, no stack arguments, plain ret; prologue 55 8B EC 83 E4 F8): if +0x874 is 0 it returns at once. Otherwise it memsets the raw block of BOTH pads
 *     (+0x198 and +0x464, 0x2C each), ORs the keyboard / mouse state into the block of the CURRENT slot (+0x870) only (held, previous, pressed, released, +0x14, sticks), writes the
 *     mouse cursor words +0x600..+0x60C and calls FUN_0088BB10(current slot) (PadData[current]+0x71). Nothing in it touches the block of a pad that is not current except the memset.
 * So keeping the block of the partner's pad across the call is the same as skipping only that pad's memset: the keyboard still drives the current slot (menus, the pause menu,
 * keyboard play), and the partner's pad keeps what the poll read (stock pad behaviour for that slot, edges included). The old kbskip skipped the whole function: no keyboard at all
 * (duo7: title never left; duo6-9: Esc opened the pause menu and nothing could close it).
 */
#ifndef KBSKIP1_H
#define KBSKIP1_H

#include <stdint.h>
#include <string.h>

#define KB1_PADS 2
#define KB1_RAW_OFF 0x198u      /* sGamePad + this + KB1_RAW_STRIDE * pad = raw block of the pad */
#define KB1_RAW_STRIDE 0x2CCu
#define KB1_RAW_SIZE 0x2Cu
#define KB1_CUR_OFF 0x870u      /* current pad slot (0 outside capture's swapped sections) */
#define KB1_MODE_OFF 0x874u     /* keyboard mode */
#define KB1_FN 0x88CB10u        /* the keyboard-mode function */
#define KB1_DEFAULT_MASK 2u     /* bit k = pad slot k is kept: pad slot 1 = the partner's (puppet's) pad; this PC always plays on pad 0 */

#if defined(__i386__)
#define KB1_FC __attribute__((fastcall))
#else
#define KB1_FC
#endif
typedef void (KB1_FC *kb1_fn)(uintptr_t self);   /* FUN_0088CB10 through its trampoline: ecx = sGamePad */

typedef struct {
    uint32_t calls;      /* calls of the function while kbskip asked to keep a pad */
    uint32_t kbcalls;    /* ... of them in keyboard mode (the frames in which the engine wipes the pads) */
    uint32_t kept;       /* frames in which a kept block was put back */
    uint32_t cur_hit;    /* keyboard-mode frames in which the CURRENT slot was a kept pad (its keyboard input of that frame is dropped: a swapped section was open) */
    uint32_t last_cur;   /* current slot seen in the last keyboard-mode frame */
} kb1_stats;

typedef struct {
    uint32_t mask;
    uint8_t raw[KB1_PADS][KB1_RAW_SIZE];
} kb1_keep;

static inline uint32_t kb1_rd32(const uint8_t *g, uint32_t off)
{
    uint32_t v;
    memcpy(&v, g + off, 4);
    return v;
}

/* before the engine function: copy the raw block of every pad in mask, only in keyboard mode (otherwise the function returns at once and nothing has to be kept) */
static inline uint32_t kb1_before(kb1_keep *k, const uint8_t *g, uint32_t mask, kb1_stats *st)
{
    k->mask = 0;
    mask &= (1u << KB1_PADS) - 1;
    if (!g || !mask)
        return 0;
    if (st)
        st->calls++;
    if (!kb1_rd32(g, KB1_MODE_OFF))
        return 0;
    uint32_t cur = kb1_rd32(g, KB1_CUR_OFF);
    if (st) {
        st->kbcalls++;
        st->last_cur = cur;
        if (cur < KB1_PADS && (mask >> cur & 1))
            st->cur_hit++;
    }
    for (unsigned s = 0; s < KB1_PADS; s++)
        if (mask >> s & 1)
            memcpy(k->raw[s], g + KB1_RAW_OFF + KB1_RAW_STRIDE * s, KB1_RAW_SIZE);
    k->mask = mask;
    return mask;
}

/* after it: the kept blocks back (the wipe of those pads is undone; the current slot keeps the keyboard input unless it is itself a kept pad) */
static inline void kb1_after(const kb1_keep *k, uint8_t *g, kb1_stats *st)
{
    if (!k->mask || !g)
        return;
    for (unsigned s = 0; s < KB1_PADS; s++)
        if (k->mask >> s & 1)
            memcpy(g + KB1_RAW_OFF + KB1_RAW_STRIDE * s, k->raw[s], KB1_RAW_SIZE);
    if (st)
        st->kept++;
}

/* the whole hook body: the stock function, with the pads of mask kept through it; mask 0 = the stock function alone */
static inline void kb1_call(kb1_fn orig, uintptr_t self, uint32_t mask, kb1_stats *st)
{
    kb1_keep k;
    if (!kb1_before(&k, (const uint8_t *)self, mask, st)) {
        orig(self);
        return;
    }
    orig(self);
    kb1_after(&k, (uint8_t *)self, st);
}

/* "1", "0", "0+1" for a mask (console text) */
static inline const char *kb1_mask_name(uint32_t mask)
{
    switch (mask & 3u) {
    case 1: return "0";
    case 2: return "1";
    case 3: return "0+1";
    default: return "none";
    }
}

#endif
