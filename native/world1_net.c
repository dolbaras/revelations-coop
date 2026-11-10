#include "world1_net.h"

#include <string.h>

static void send_cb(void *u, int lane, unsigned kind, const uint8_t *body, unsigned len)
{
    w1n_t *n = (w1n_t *)u;
    int ok = lane == W1_LANE_EVENT ? cs_send_event(n->cs, kind, body, len) : cs_send_world(n->cs, kind, body, len);
    if (!ok) n->tx_refused++;   /* not PLAYING / no session: the model retransmits events, world records are repeated by the next batch */
}

void w1n_init(w1n_t *n, cs_t *cs, int role, int own_slot, const w1_ops *ops)
{
    memset(n, 0, sizeof *n);
    n->cs = cs;
    w1_ops o;
    memset(&o, 0, sizeof o);
    if (ops) o = *ops;
    o.send = send_cb, o.send_user = n;
    w1_init(&n->w, role, own_slot, &o);
}

void w1n_note(w1n_t *n, const cs_note *note)
{
    if (note->what != CS_N_EVENT || !note->ev || note->ev->kind != CP_EV_W1) return;
    n->rx_events++;
    w1_recv(&n->w, W1_LANE_EVENT, CP_EV_W1, note->ev->body, note->ev->len);
}

void w1n_frame(w1n_t *n)
{
    if (cs_phase(n->cs) != CP_PHASE_PLAYING) return;
    w1_set_epoch(&n->w, cs_epoch(n->cs));
    cp_world wd;
    while (cs_recv_world(n->cs, &wd)) {
        n->rx_world++;
        w1_recv(&n->w, W1_LANE_WORLD, wd.kind, wd.body, wd.len);
    }
    w1_tick(&n->w, cs_now(n->cs));
}
