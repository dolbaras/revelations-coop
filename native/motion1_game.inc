/*
 * motion1_game.inc - included by world1_game.c after hitx1_game.inc: rev1 live round 14 (task 2), a replica enemy plays its owner's motions (mot1.h).
 *   the PC that runs an enemy on its own AI (the host: its kind-2 enemies; the client: its local1 "local" enemies) reads the motion id of layers 0..2 of
 *   each such unit at ~30 Hz and sends a changed layer (ext CP_W1_MOT); the other PC sets it on its replica (kind 3) through the unit's own vtable slot 27
 *   (0x6CD200 -> uModel_vf27): only the picture. `world set motion 0|1`; `world motion` = counters.
 */
#include "mot1.h"

static volatile int f_motion = 1;
static mo_t MOT, MOR;                          /* sender / receiver (logic thread) */
static uint16_t mo_seen_key[MO_MAX];
static uint32_t mo_seen_unit[MO_MAX];
static unsigned mo_nseen;
static uint32_t mo_last_ms;
static struct { unsigned passes, tx, tx_fail, rx, bad, applied, old, no_replica, vf_refused, lines; } MOS;
typedef unsigned (TC *setmot_fn)(void *unit, int layer, unsigned id, uint32_t blend_bits, float frame, uint32_t speed_bits, unsigned flags);
#define MO_VF27 0x6CD200u

static void mo_reset_all(void)
{
    mo_reset(&MOT), mo_reset(&MOR);
    mo_nseen = 0;
}

static unsigned rd16(uint32_t a)
{
    uint16_t v = 0;
    if (a) MEM.read(MEM.user, a, &v, 2);
    return v;
}

/* the sender (both PCs, logic thread, model lock) */
static void motion_pass(uint32_t now)
{
    if (!f_motion || (int32_t)(now - mo_last_ms) < 33) return;
    mo_last_ms = now;
    uint32_t sa = sarrange();
    if (!sa) return;
    MOS.passes++;
    unsigned n = wh_list_count(&MEM, sa, WH_LIST_ENEMIES), sent = 0;
    for (unsigned i = 0; i < n && sent < 12; i++) {
        wh_entry e;
        unsigned b = 0, ix = 0;
        if (!wh_list_at(&MEM, sa, WH_LIST_ENEMIES, i, &e) || !e.unit || !enemy_unit(e.unit)) continue;
        if (rd32(e.unit + WH_U_KIND) != 2 || !(rdf(e.unit + WH_U_HP) > 0.0f) || (rd32(e.unit + WH_U_FLAGS) & (LR_E1C_HIDDEN | 0x80000000u))) continue;
        if (!wh_entry_key(&MEM, sa, &e, &b, &ix) || b > 255 || ix > 255) continue;
        w1_key key = W1_KEY(b, ix);
        if (g_client) {   /* the client sends only for its local enemies (local1.h): the others are the host's */
            lr_ent *le = lr_find(&LR, key, 0);
            if (!le || !le->local) continue;
        } else if (w1_enemy_state(&N.w, key) != W1_ALIVE) continue;
        {   /* a new unit for the key: everything again */
            unsigned k;
            for (k = 0; k < mo_nseen && mo_seen_key[k] != key; k++) {}
            if (k == mo_nseen && mo_nseen < MO_MAX) mo_seen_key[mo_nseen] = key, mo_seen_unit[mo_nseen++] = e.unit;
            else if (k < mo_nseen && mo_seen_unit[k] != e.unit) mo_seen_unit[k] = e.unit, mo_tx_forget(&MOT, key);
        }
        for (unsigned l = 0; l < MO_LAYERS; l++) {
            uint32_t ly = e.unit + MO_LAYER_OFF + l * MO_LAYER_SIZE;
            mo_msg m;
            m.id = (uint16_t)rd16(ly + 4);
            if (!mo_tx_step(&MOT, key, l, m.id, &m.seq)) continue;
            m.key = key, m.layer = (uint8_t)l, m.flags = rd32(ly + 0xC) & MO_FLAGS_MASK, m.blend = rdf(ly + 0x40), m.frame = rdf(ly + 0x4C), m.speed = rdf(ly + 0x5C);
            if (!isfinite(m.blend)) m.blend = 0.0f;
            if (!isfinite(m.frame) || fabsf(m.frame) > 1.0e6f) m.frame = 0.0f;
            if (!isfinite(m.speed) || fabsf(m.speed) > 100.0f) m.speed = 1.0f;
            uint8_t buf[MO_LEN];
            mo_enc(buf, &m);
            if (w1_send_ext(&N.w, CP_W1_MOT, buf, MO_LEN)) MOS.tx++, sent++;
            else MOS.tx_fail++, mo_tx_forget(&MOT, key);   /* not queued: tried again next pass */
            if (MOS.tx == 1 || (MOS.tx <= 400 && MOS.tx % 100 == 0))
                wlog("motion: enemy %u/%u (unit %08x, own AI) layer %u plays motion %04x (frame %.1f, speed %.2f): sent to the other PC (%u sent)", b, ix, (unsigned)e.unit, l, m.id, (double)m.frame, (double)m.speed, MOS.tx);
        }
    }
}

/* the receiver: the replica plays it (op_ext, logic thread, model lock) */
static void motion_rx(const uint8_t *b, unsigned len)
{
    mo_msg m;
    MOS.rx++;
    if (!mo_dec(b, len, &m)) {
        MOS.bad++;
        return;
    }
    if (!f_motion || !mo_rx_step(&MOR, &m)) {
        MOS.old++;
        return;
    }
    uint32_t unit = unit_of_key(m.key), fn = 0;
    if (!unit || rd32(unit + WH_U_KIND) != 3 || !(rdf(unit + WH_U_HP) > 0.0f)) {
        MOS.no_replica++;
        return;
    }
    if (!wh_unit_vfn(&MEM, unit, 0x6C, &fn) || fn != MO_VF27) {
        if (++MOS.vf_refused <= 10) wlog("motion: replica %u/%u unit %08x: its slot 27 is %08x, not the verified 0x6CD200: left alone", m.key >> 8, m.key & 0xFF, (unsigned)unit, (unsigned)fn);
        return;
    }
    uint32_t bb, sb;
    memcpy(&bb, &m.blend, 4), memcpy(&sb, &m.speed, 4);
    ops_enter();
    ((setmot_fn)(uintptr_t)fn)((void *)(uintptr_t)unit, m.layer, m.id, bb, m.frame, sb, m.flags);
    ops_leave();
    MOS.applied++;
    if (MOS.applied == 1 || (MOS.applied <= 400 && MOS.applied % 100 == 0))
        wlog("motion: replica %u/%u (unit %08x) layer %u plays the owner's motion %04x from frame %.1f (%u applied)", m.key >> 8, m.key & 0xFF, (unsigned)unit, m.layer, m.id, (double)m.frame, MOS.applied);
}

static void motion_status(char *out, int size)
{
    snprintf(out, (size_t)size, "world motion (mot1.h): motion %d | sender: passes %u, sent %u (not queued %u), keys %u | receiver: received %u (bad %u, old %u), applied %u, no replica %u, slot 27 refused %u",
             f_motion, MOS.passes, MOS.tx, MOS.tx_fail, mo_nseen, MOS.rx, MOS.bad, MOS.old, MOS.applied, MOS.no_replica, MOS.vf_refused);
}
