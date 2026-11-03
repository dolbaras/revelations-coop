/*
 * Input record and input sources for a human-controlled character ("captured" partner, later a remote player).
 *
 * One record = what one pad sent in one frame, in the layout of XINPUT_GAMEPAD (12 bytes). That is exactly what the game's own pad code
 * (sPad_PollPad -> FUN_009bde20) turns into its logical button mask, sticks and triggers, so a record from the network is fed to the game
 * through the same funnel as a real pad: the game's own readers see it, with its own dead zones, run/aim rules and button mapping.
 *
 *   wire format (network layer, 12 bytes, little endian):  u16 buttons | u8 lt | u8 rt | i16 lx | i16 ly | i16 rx | i16 ry
 *   (a compact 8-byte form is possible: sticks as i8 = value >> 8; the plug-in keeps the full 12 bytes)
 *
 * A source says where the record of a slot comes from, evaluated once per frame on the game thread:
 *   R1_SRC_PAD     the local pad of XInput user <arg> (real, or a synthetic test pad of the console `pad` command)
 *   R1_SRC_REMOTE  the last record handed in with r1in_remote_set(slot, rec) (the network thread calls it; one writer, torn reads are harmless)
 *   R1_SRC_SCRIPT  a timed list of records (tests / bots), see plug_cap.c `P script`
 *
 * Injection point (how a record reaches the game): the record is published as the state of the synthetic XInput user <slot>
 * (testmode_pad_set), the game polls it into Pad[slot] and builds PadData[slot] (padfix hook of FUN_0088A470); the captured unit's update then runs with
 * sGamePad+0x870 = slot (swap), so EVERY reader of the unit (move, aim, fire, reload, melee, item, interact, look) reads the record.
 * The pad-group writer ("mode 1", PR #115 recipe) builds the unit's move/rotate/aim floats from the same record after updatePad; it cannot carry
 * the buttons (the fire/reload/... readers are inline reads of Pad[+0x870].held at dozens of sites), so it is used on top of the swap, not instead.
 */
#ifndef COOP_INPUT_H
#define COOP_INPUT_H
#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t buttons;   /* XINPUT_GAMEPAD_* mask */
    uint8_t lt, rt;     /* triggers 0..255 */
    int16_t lx, ly, rx, ry;
} r1_input_t;           /* == XINPUT_GAMEPAD, 12 bytes */
#pragma pack(pop)

enum { R1_SRC_PAD = 0, R1_SRC_REMOTE = 1, R1_SRC_SCRIPT = 2 };

typedef struct {
    int kind;
    int arg;                 /* R1_SRC_PAD: XInput user */
    r1_input_t last;         /* last record delivered (R1_SRC_REMOTE: the one to use) */
    unsigned seq;            /* number of records received */
} r1_input_src_t;

#endif
