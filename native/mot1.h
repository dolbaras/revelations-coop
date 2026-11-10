/*
 * mot1.h - rev1 live round 14 (task 2): a REPLICA enemy plays the motions its owner's PC plays. Pure (no Windows, no engine): the wire, the sender's
 * change detection, the receiver's order; motion1_game.inc runs it, tests/test_mot1.c drives it.
 *
 * FACTS (Ghidra l14 o4 / o37 / o38):
 *   - a kind-3 replica never changes state (the zombie's update requests its states FUN_0049C790 only when local, FUN_006C9FE0), so it keeps the pose of its
 *     spawn state (round 13: "the replica zombie stands in a default pose");
 *   - the motion of a unit is set by its vtable slot 27 (+0x6C; 0x6CD200 for the enemies: the cNetInterpMove gate FUN_0049F560 answers 1 offline, then
 *     uModel_vf27 0xA32480) = setMotion(layer 0..2, motion id (bank << 8 | index, 0xFFFF = none), blend frames, start frame, speed, flags);
 *     the layer's record is at unit + 0x2D0 + layer * 0x140: +0x04 u16 id, +0x0C flags, +0x40 blend, +0x4C current frame, +0x5C speed.
 * THE RULE: the PC that runs an enemy on its own AI (kind 2: the host's enemies, the client's local1 enemies) sends a layer's motion when its id changes
 * (ext CP_W1_MOT, sequence numbered per key and layer); the PC that shows it as a replica (kind 3) sets the same motion through the unit's own slot 27
 * (only the picture: no state, no AI, no attack). The death keeps its own path (hitx1.h: the replica becomes local and dies by its own update).
 */
#ifndef MOT1_H
#define MOT1_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MO_LAYERS 3
#define MO_LEN 22                 /* key u16, layer u8, seq u8, id u16, flags u32, blend f32, frame f32, speed f32 */
#define MO_LAYER_OFF 0x2D0u
#define MO_LAYER_SIZE 0x140u
#define MO_FLAGS_MASK 0x0000FFFFu /* the caller's flag bits (0x20000 = relative frame is not sent: the frame is absolute) */
#define MO_MAX 64

typedef struct { uint16_t key; uint8_t layer, seq; uint16_t id; uint32_t flags; float blend, frame, speed; } mo_msg;

static inline unsigned mo_enc(uint8_t *b, const mo_msg *m)
{
    b[0] = (uint8_t)(m->key & 0xFF), b[1] = (uint8_t)(m->key >> 8), b[2] = m->layer, b[3] = m->seq, b[4] = (uint8_t)(m->id & 0xFF), b[5] = (uint8_t)(m->id >> 8);
    memcpy(b + 6, &m->flags, 4), memcpy(b + 10, &m->blend, 4), memcpy(b + 14, &m->frame, 4), memcpy(b + 18, &m->speed, 4);
    return MO_LEN;
}
static inline int mo_dec(const uint8_t *b, unsigned len, mo_msg *m)
{
    if (!b || len != MO_LEN) return 0;
    m->key = (uint16_t)(b[0] | (b[1] << 8)), m->layer = b[2], m->seq = b[3], m->id = (uint16_t)(b[4] | (b[5] << 8));
    memcpy(&m->flags, b + 6, 4), memcpy(&m->blend, b + 10, 4), memcpy(&m->frame, b + 14, 4), memcpy(&m->speed, b + 18, 4);
    if (m->layer >= MO_LAYERS || !isfinite(m->blend) || !isfinite(m->frame) || !isfinite(m->speed) || fabsf(m->frame) > 1.0e6f || fabsf(m->speed) > 100.0f) return 0;
    return 1;
}

/* per key: the sender's last id per layer + sequence; the receiver's last sequence per layer */
typedef struct { uint16_t key; uint8_t used, have[MO_LAYERS], seq[MO_LAYERS]; uint16_t id[MO_LAYERS]; } mo_ent;
typedef struct { mo_ent e[MO_MAX]; unsigned sent, applied, old, full; } mo_t;
static inline void mo_reset(mo_t *t) { memset(t, 0, sizeof *t); }
static inline mo_ent *mo_find(mo_t *t, uint16_t key, int create)
{
    mo_ent *fr = 0;
    for (unsigned i = 0; i < MO_MAX; i++) {
        if (t->e[i].used && t->e[i].key == key) return &t->e[i];
        if (!t->e[i].used && !fr) fr = &t->e[i];
    }
    if (!create) return 0;
    if (!fr) { t->full++; return 0; }
    memset(fr, 0, sizeof *fr), fr->used = 1, fr->key = key;
    return fr;
}

/* sender (the PC that runs the enemy): 1 = send this layer now (its id changed or was never sent); fills the sequence */
static inline int mo_tx_step(mo_t *t, uint16_t key, unsigned layer, uint16_t id, uint8_t *seq)
{
    if (layer >= MO_LAYERS) return 0;
    mo_ent *e = mo_find(t, key, 1);
    if (!e) return 0;
    if (e->have[layer] && e->id[layer] == id) return 0;
    e->have[layer] = 1, e->id[layer] = id, *seq = ++e->seq[layer];
    t->sent++;
    return 1;
}
/* the sender forgets what it sent for a key whose unit changed (a new unit plays from its own start: everything is sent again); the sequences go on
 * (a restart at 1 would be "older" than what the receiver applied last) */
static inline void mo_tx_forget(mo_t *t, uint16_t key)
{
    mo_ent *e = mo_find(t, key, 0);
    if (e) memset(e->have, 0, sizeof e->have);
}

/* receiver (the PC that shows the replica): 1 = apply (newer than the last applied of this key and layer) */
static inline int mo_rx_step(mo_t *t, const mo_msg *m)
{
    mo_ent *e = mo_find(t, m->key, 1);
    if (!e) return 0;
    if (e->have[m->layer] && (int8_t)(uint8_t)(m->seq - e->seq[m->layer]) <= 0) {
        t->old++;
        return 0;
    }
    e->have[m->layer] = 1, e->seq[m->layer] = m->seq, e->id[m->layer] = m->id;
    t->applied++;
    return 1;
}

#endif
