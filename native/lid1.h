/*
 * lid1.h - sync022 rows W2 / W3 (notes\net-sync-max.md): the door of a locker (uOpenBox: uLockerDoor, uTerragrigiaLockerDoor, uOMCorpseBox, uOMSafeDoor) or the
 * lid of a weapon box (uWeaponBox) the partner opens shows the opening on this PC too. A PICTURE mirror: the copy here stays logically closed (its state
 * machine is never asked: a replay would spawn a second content item here (uOpenBox Opening enter 0x66DD50 -> FUN_005D7C30) or open the weapon GUI here
 * (uWeaponBox Opened 0x680460)); only its layer-0 motion plays the partner's opening / closing through the object's own setter (vtable slot 26 0x4BBA80 ->
 * slot 27 0x6CD200 with layer 0). Pure (no Windows, no engine): the wire, the sender's change detection, the receiver's decision; lid1_game.inc runs them,
 * tests/test_lid1.c drives them on two PCs.
 *
 * FACTS (the image, work\s22-out; tests/test_lid1.c checks the bytes):
 *   - each state sets its motion in its enter (class vtable slot 5) by slot 26 (id, blend 0, frame 0, speed 1.0, flags 4): uOpenBox Closed 0 (0x66CFB0),
 *     Opening 1 (0x66DD50), Opened 2 (0x66D5B0), Closing 3 (0x66D7A0); uWeaponBox Closed 0 (0x67FB60), Opening 1 (0x67FDD0), Closing 3 (0x680150);
 *   - the Closed states' per-frame calls (uOpenBox vf8 0x66D030, vf9 0x66D0A0; uWeaponBox vf9 0x67FBE0) set no motion: a picture on a closed copy stays;
 *   - a box here is opened only by this PC's own human (the objects take the own slot as their operator: FUN_00882D60 / FUN_0067AF10): every Opening /
 *     Closing seen on this PC is its own human's.
 * THE RULE: the sender (each PC, every pass) watches the boxes' states; a box that ENTERS Opening / Closing here (not at its first sight) -> LID{class, key4,
 * OPEN / CLOSE, motion id} (ext CP_W1_LID, reliable, once per entry). The receiver plays that motion on its copy only when the copy is the same class, is IN ITS
 * CLOSED STATE (this PC's own human is not using it and has not opened it) and the id is the open (1) / close (3) motion; a CLOSE only on a copy that shows
 * our OPEN (its layer-0 id is still 1). Nothing else is called: no state, no item, no GUI, no flag.
 * LIMITS (release notes): the content stays per PC (each player loots his own copy, as before); a copy shown open is still closed for this PC's player: when he
 * opens it the stock opening plays again from the closed pose; its collision stays closed (the door is a picture).
 */
#ifndef LID1_H
#define LID1_H

#include <stdint.h>
#include <string.h>

#define LD_LEN 24          /* ver u8, kind u8, id u16, class u32, key4 4 x u32 */
#define LD_VER 1
#define LD_MAX 96          /* boxes followed per PC (a section has at most ~30) */
#define LD_ID_CLOSED 0
#define LD_ID_OPEN 1
#define LD_ID_CLOSE 3

enum { LD_OPEN = 1, LD_CLOSE = 2 };
/* a box's state as a tag (the glue maps the DTI ids) */
enum { LD_S_UNKNOWN = 0, LD_S_CLOSED, LD_S_OPENING, LD_S_OPENED, LD_S_CLOSING, LD_S_OTHER };
/* the receiver's answers */
enum { LD_PLAY = 1, LD_R_CLASS = -1, LD_R_BUSY = -2, LD_R_ID = -3, LD_R_NOTSHOWN = -4, LD_R_NOCOPY = -5 };

typedef struct { uint8_t kind; uint16_t id; uint32_t cls; uint32_t k4[4]; } ld_msg;

static inline unsigned ld_enc(uint8_t *b, const ld_msg *m)
{
    b[0] = LD_VER, b[1] = m->kind, b[2] = (uint8_t)m->id, b[3] = (uint8_t)(m->id >> 8);
    memcpy(b + 4, &m->cls, 4), memcpy(b + 8, m->k4, 16);
    return LD_LEN;
}
static inline int ld_dec(const uint8_t *b, unsigned len, ld_msg *m)
{
    if (!b || len != LD_LEN || b[0] != LD_VER || (b[1] != LD_OPEN && b[1] != LD_CLOSE)) return 0;
    m->kind = b[1], m->id = (uint16_t)(b[2] | (b[3] << 8));
    memcpy(&m->cls, b + 4, 4), memcpy(m->k4, b + 8, 16);
    return m->cls != 0 && (m->kind == LD_OPEN ? m->id == LD_ID_OPEN : m->id == LD_ID_CLOSE);
}

/* ---- the sender: the last tag of each box seen here */
typedef struct {
    struct { uint32_t k4[4]; int tag; unsigned pass; int used; } e[LD_MAX];
    unsigned pass, opens, closes, first_seen, full;
} ld_tx;
static inline void ld_tx_reset(ld_tx *t) { memset(t, 0, sizeof *t); }
static inline void ld_tx_pass(ld_tx *t) { t->pass++; }
/* one box this pass: 1 = *out is a message to send */
static inline int ld_tx_step(ld_tx *t, const uint32_t k4[4], uint32_t cls, int tag, uint16_t id, ld_msg *out)
{
    int fr = -1, old = -1;
    unsigned oldp = 0xFFFFFFFFu;
    for (int i = 0; i < LD_MAX; i++) {
        if (t->e[i].used && !memcmp(t->e[i].k4, k4, 16)) {
            int prev = t->e[i].tag;
            t->e[i].tag = tag, t->e[i].pass = t->pass;
            if (tag == prev) return 0;
            if (tag == LD_S_OPENING) {
                out->kind = LD_OPEN, out->id = id, out->cls = cls, memcpy(out->k4, k4, 16), t->opens++;
                return id == LD_ID_OPEN;
            }
            if (tag == LD_S_CLOSING) {
                out->kind = LD_CLOSE, out->id = id, out->cls = cls, memcpy(out->k4, k4, 16), t->closes++;
                return id == LD_ID_CLOSE;
            }
            return 0;
        }
        if (!t->e[i].used && fr < 0) fr = i;
        if (t->e[i].used && t->e[i].pass < oldp) oldp = t->e[i].pass, old = i;
    }
    if (fr < 0) fr = old, t->full++;   /* the oldest entry gives way */
    if (fr < 0) return 0;
    t->e[fr].used = 1, memcpy(t->e[fr].k4, k4, 16), t->e[fr].tag = tag, t->e[fr].pass = t->pass, t->first_seen++;
    return 0;   /* the first sight never sends (a box already open when this PC came in) */
}

/* ---- the receiver */
typedef struct { unsigned rx, bad, played_open, played_close, r_class, r_busy, r_id, r_notshown, r_nocopy; } ld_rx;
/* copy_tag / copy_id = the copy's state tag and layer-0 motion id here; same_class = the copy is the message's class with the verified setter */
static inline int ld_rx_decide(ld_rx *r, const ld_msg *m, int have_copy, int same_class, int copy_tag, uint16_t copy_id)
{
    if (!have_copy) return r->r_nocopy++, LD_R_NOCOPY;
    if (!same_class) return r->r_class++, LD_R_CLASS;
    if (copy_tag != LD_S_CLOSED) return r->r_busy++, LD_R_BUSY;   /* this PC's own human uses it / opened it: his state machine owns the picture */
    if (m->kind == LD_OPEN) {
        if (m->id != LD_ID_OPEN) return r->r_id++, LD_R_ID;
        r->played_open++;
        return LD_PLAY;
    }
    if (m->id != LD_ID_CLOSE) return r->r_id++, LD_R_ID;
    if (copy_id != LD_ID_OPEN) return r->r_notshown++, LD_R_NOTSHOWN;   /* the copy does not show our OPEN (never got it, or its own Closed enter put it back) */
    r->played_close++;
    return LD_PLAY;
}

#endif
