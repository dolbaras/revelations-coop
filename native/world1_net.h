/*
 * world1_net: glue between the world model (world1.c) and the co-op session (coop_session.c). No Windows API of its own, no game dependencies.
 *  - the model's reliable lane is cs_send_event(CP_EV_W1, ...), its unreliable lane cs_send_world(kind, ...);
 *  - incoming events come from the session's notification (CS_N_EVENT, call w1n_note from cs_config.on_note), incoming world records from cs_recv_world;
 *  - the model's epoch is the session's level instance counter (cs_epoch) and is taken over when the session is PLAYING; a message of another epoch is refused.
 * Call w1n_frame() once per game frame (the logic thread). Outside PLAYING (loading) nothing is sent or applied and the old level's state stays until the new epoch starts.
 */
#ifndef WORLD1_NET_H
#define WORLD1_NET_H

#include "coop_session.h"
#include "world1.h"

typedef struct {
    w1_t w;
    cs_t *cs;
    uint32_t rx_events, rx_world, tx_refused;
} w1n_t;

/* ops: the engine side callbacks (spawn, update, kill, ...); send is set here. role W1_HOST / W1_CLIENT, own_slot 0 lead / 1 partner. */
void w1n_init(w1n_t *n, cs_t *cs, int role, int own_slot, const w1_ops *ops);
void w1n_note(w1n_t *n, const cs_note *note);        /* from on_note: takes CS_N_EVENT of kind CP_EV_W1, ignores everything else */
void w1n_frame(w1n_t *n);                              /* epoch from the session, inbox of world records, timers */

#endif
