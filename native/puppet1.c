/*
 * puppet1.c - the logic of the remote human of the Revelations 1 network co-op, see puppet1.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "puppet1.h"

static int32_t tdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
static int iabs(int v) { return v < 0 ? -v : v; }

/* ================================================================== sender */
void p1_tx_defaults(p1_tx_cfg *c)
{
    c->keepalive_ms = 50, c->min_interval_ms = 15, c->stick_eps = 192, c->redundancy = 3;
}

void p1_tx_init(p1_tx *t, const p1_tx_cfg *c)
{
    memset(t, 0, sizeof *t);
    if (c) t->cfg = *c;
    else p1_tx_defaults(&t->cfg);
    if (t->cfg.redundancy < 1) t->cfg.redundancy = 1;
    if (t->cfg.redundancy > CP_PAD_MAX_REC) t->cfg.redundancy = CP_PAD_MAX_REC;
}

void p1_tx_restart(p1_tx *t)
{
    t->n = 0, t->seq = 0, t->have_send = 0, t->last_send = 0, t->last_flags = 0;
}

static int stick_changed(int cur, int old, int eps)
{
    if ((cur == 0) != (old == 0)) return 1;   /* a stick that returns to rest is always sent, whatever the tolerance */
    return iabs(cur - old) > eps;
}

int p1_tx_step(p1_tx *t, uint32_t now, const cp_pad_rec *cur, uint16_t flags, cp_pad *out)
{
    int changed = 1;
    if (t->n > 0) {
        const cp_pad_rec *h = &t->hist[0].r;
        int eps = t->cfg.stick_eps < 0 ? 0 : t->cfg.stick_eps;
        changed = cur->buttons != h->buttons || cur->lt != h->lt || cur->rt != h->rt || stick_changed(cur->lx, h->lx, eps) || stick_changed(cur->ly, h->ly, eps) ||
                  stick_changed(cur->rx, h->rx, eps) || stick_changed(cur->ry, h->ry, eps);
    }
    int send = 0;
    if (changed && (t->n == 0 || tdiff(now, t->hist[0].t) >= t->cfg.min_interval_ms)) {
        memmove(&t->hist[1], &t->hist[0], sizeof t->hist[0] * (CP_PAD_MAX_REC - 1));
        t->hist[0].r = *cur, t->hist[0].t = now;
        if (t->n < CP_PAD_MAX_REC) t->n++;
        t->seq = (uint16_t)(t->seq + 1);
        t->records++;
        send = 1;
    } else if (t->have_send && flags != t->last_flags) {
        send = 1;   /* a hold flag changed: tell the partner at once */
    } else if (t->n > 0 && (!t->have_send || tdiff(now, t->last_send) >= t->cfg.keepalive_ms)) {
        send = 1;
        t->keepalives++;
    }
    if (!send) return 0;
    memset(out, 0, sizeof *out);
    out->flags = flags, out->seq = t->seq, out->t_ms = t->hist[0].t;
    int n = t->n < t->cfg.redundancy ? t->n : t->cfg.redundancy;
    out->count = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        out->rec[i] = t->hist[i].r;
        if (i) {
            int32_t d = tdiff(t->hist[i - 1].t, t->hist[i].t);
            out->dt[i] = (uint8_t)(d < 0 ? 0 : d > 255 ? 255 : d);
        }
    }
    t->have_send = 1, t->last_send = now, t->last_flags = flags;
    t->packets++;
    return 1;
}

/* ================================================================== receiver */
void p1_rx_defaults(p1_rx_cfg *c)
{
    c->neutral_ms = 400, c->deadman_ms = 10000, c->hold_repeat_ms = 500, c->state_stale_ms = 600;
}

void p1_rx_init(p1_rx *r, const p1_rx_cfg *c, p1_sink sink)
{
    memset(r, 0, sizeof *r);
    if (c) r->cfg = *c;
    else p1_rx_defaults(&r->cfg);
    r->sink = sink;
    r->link = P1_LINK_IDLE, r->mode = P1_MODE_RUN;
}

static void push_rec(p1_rx *r, const cp_pad_rec *rec, uint32_t ts)
{
    uint32_t seq = ++r->out_seq;
    int ok = r->sink.push ? r->sink.push(r->sink.user, rec, seq, ts) : 0;
    if (ok) r->st.pushed++;
    else r->st.rejected++;
}

static void push_neutral(p1_rx *r, uint32_t now)
{
    cp_pad_rec z;
    memset(&z, 0, sizeof z);
    push_rec(r, &z, now);
    r->st.neutral_pushes++;
    r->neutral_at = now;
}

/* a new level instance (or the first packet): the sequence numbers, the pending records and the target belong to the old one */
static void reset_epoch(p1_rx *r, unsigned epoch)
{
    r->epoch = (uint8_t)epoch, r->have_epoch = 1;
    r->have_seq = 0, r->npend = 0, r->have_latest = 0, r->have_rx = 0;
    r->tgt.valid = 0, r->soft_on = 0, r->tele_pending = 0, r->have_tele_stamp = 0;
    r->need_tp = 1;   /* the units are new: the first fresh sample places the puppet */
    r->pkt_flags = 0;
}

static void track_latency(p1_rx *r, uint32_t now, uint32_t stamp)
{
    int32_t off = tdiff(now, stamp);
    if (!r->have_off) {
        r->off_min = r->off_win_min = off, r->off_win_start = now, r->have_off = 1;
    }
    if (off < r->off_win_min) r->off_win_min = off;
    if (off < r->off_min) r->off_min = off;
    if (tdiff(now, r->off_win_start) > 5000) {   /* the clocks drift: the best case is the minimum of the last window */
        r->off_min = r->off_win_min;
        r->off_win_min = off, r->off_win_start = now;
    }
    int32_t lat = off - r->off_min;
    if (lat < 0) lat = 0;
    r->lat_ewma = r->lat_ewma * 0.9f + (float)lat * 0.1f;
    if ((uint32_t)lat > r->lat_max) r->lat_max = (uint32_t)lat;
}

void p1_rx_packet(p1_rx *r, uint32_t now, const cp_pad *pkt, unsigned cur_epoch)
{
    if (pkt->count < 1 || pkt->count > CP_PAD_MAX_REC) return;
    if (pkt->epoch != (uint8_t)cur_epoch) {
        r->st.wrong_epoch++;
        return;
    }
    if (!r->have_epoch || r->epoch != pkt->epoch) reset_epoch(r, pkt->epoch);
    r->st.packets++;
    r->last_rx = now, r->have_rx = 1, r->alive = 1;
    uint32_t newest;
    int32_t delta;
    if (!r->have_seq) {
        /* the first packet of an epoch: its whole window is new; nothing older is known to be missing */
        newest = 0x10000u + pkt->seq;
        r->ext_hi = newest, r->applied_hi = newest - pkt->count, r->have_seq = 1, delta = 1;
    } else {
        delta = (int16_t)(uint16_t)(pkt->seq - (uint16_t)r->ext_hi);
        newest = r->ext_hi + (uint32_t)delta;
        if (delta > 0) r->ext_hi = newest;
    }
    if (delta >= 0) r->pkt_flags = pkt->flags;
    int first_new = 1;
    uint32_t t_rec[CP_PAD_MAX_REC];
    t_rec[0] = pkt->t_ms;
    for (int i = 1; i < pkt->count; i++) t_rec[i] = t_rec[i - 1] - pkt->dt[i];
    int any_new = 0;
    for (int i = pkt->count - 1; i >= 0; i--) {
        uint32_t s = newest - (uint32_t)i;
        if (tdiff(s, r->applied_hi) > 0) {
            if (first_new) {
                uint32_t gap = s - r->applied_hi - 1;
                if (gap && r->st.records) r->st.lost += gap;   /* records that were never seen (outside every window we got) */
                first_new = 0;
            }
            r->applied_hi = s;
            r->st.records++;
            any_new = 1;
            r->latest = pkt->rec[i], r->have_latest = 1;
            if (r->npend == P1_PENDING) {   /* the game thread did not tick for a long time: keep the newest */
                memmove(&r->pend[0], &r->pend[1], sizeof r->pend[0] * (P1_PENDING - 1));
                r->npend--;
                r->st.pending_overflow++;
            }
            r->pend[r->npend].r = pkt->rec[i], r->pend[r->npend].seq = s, r->pend[r->npend].t = t_rec[i], r->npend++;
            track_latency(r, now, t_rec[i]);
        }
    }
    if (!any_new) {
        if (newest == r->applied_hi) r->st.keepalive_or_dup++;
        else r->st.late++;
    }
}

void p1_rx_state(p1_rx *r, uint32_t now, int valid, const float pos[3], float yaw, uint16_t flags, uint32_t t_ms, unsigned epoch)
{
    if (!valid) {
        r->tgt.valid = 0;
        return;
    }
    r->tgt.pos[0] = pos[0], r->tgt.pos[1] = pos[1], r->tgt.pos[2] = pos[2], r->tgt.yaw = yaw;
    r->tgt.flags = flags, r->tgt.epoch = (uint8_t)epoch, r->tgt.t_ms = t_ms, r->tgt.valid = 1;
    r->state_flags = flags, r->state_rx = now;
    if ((flags & CP_SF_TELEPORT) && (!r->have_tele_stamp || t_ms != r->tele_stamp)) {
        r->tele_pending = 1, r->tele_stamp = t_ms, r->have_tele_stamp = 1;
    }
}

void p1_rx_want_tp(p1_rx *r) { r->need_tp = 1; }

void p1_rx_tick(p1_rx *r, uint32_t now, unsigned cur_epoch, uint16_t local_flags)
{
    if (!r->have_epoch) r->epoch = (uint8_t)cur_epoch, r->have_epoch = 1;
    else if (r->epoch != (uint8_t)cur_epoch) reset_epoch(r, cur_epoch);

    /* link */
    int prev_link = r->link, link;
    int32_t since = r->have_rx ? tdiff(now, r->last_rx) : 0;
    if (!r->have_rx) link = P1_LINK_IDLE;
    else if (since > r->cfg.deadman_ms) link = P1_LINK_LOST;
    else if (since > r->cfg.neutral_ms) link = P1_LINK_STALLED;
    else link = P1_LINK_LIVE;
    if (link != prev_link) {
        if (link == P1_LINK_STALLED && prev_link == P1_LINK_LIVE) r->st.stalls++;
        if (link == P1_LINK_LOST) r->st.losts++;
        if (link == P1_LINK_LIVE && prev_link == P1_LINK_LOST) r->st.resumes++, r->need_tp = 1;
    }
    r->link = link;

    /* hold: the packet's flags are the freshest word of the sender; the STATE flags stand in while there are no packets */
    uint16_t pf = 0;
    if (link == P1_LINK_LIVE) pf = r->pkt_flags;
    else if (r->tgt.valid && tdiff(now, r->state_rx) <= r->cfg.state_stale_ms) pf = r->state_flags;
    uint16_t hold = (uint16_t)((pf | local_flags) & P1_HOLD_MASK);
    r->hold_flags = hold;
    int want;
    if (hold & CP_SF_CUTSCENE) want = P1_MODE_SILENT;
    else if (hold || link != P1_LINK_LIVE) want = P1_MODE_NEUTRAL;
    else want = P1_MODE_RUN;
    if (hold || link == P1_LINK_LOST) r->held_seen = 1;

    if (want != r->mode) {
        if (want != P1_MODE_RUN) {
            if (!(want == P1_MODE_NEUTRAL && link == P1_LINK_IDLE && !hold && r->mode == P1_MODE_RUN && !r->st.pushed)) push_neutral(r, now);
        } else {   /* back to running: the buttons the sender holds now, and the position */
            if (r->held_seen) r->need_tp = 1;
            r->held_seen = 0;
            if (r->have_latest && !r->npend) push_rec(r, &r->latest, now);   /* (pending records of this very packet follow below) */
        }
        r->mode = want;
    } else if (want == P1_MODE_NEUTRAL && link != P1_LINK_IDLE && tdiff(now, r->neutral_at) >= r->cfg.hold_repeat_ms) {
        push_neutral(r, now);
    }

    if (r->mode == P1_MODE_RUN) {
        for (int i = 0; i < r->npend; i++) {
            push_rec(r, &r->pend[i].r, now);
        }
        if (!r->npend && r->alive && r->have_latest && r->link == P1_LINK_LIVE) push_rec(r, &r->latest, now);   /* keepalive: the same record again */
        r->npend = 0;
    } else {
        r->st.held_dropped += (uint32_t)r->npend;
        r->npend = 0;
    }
    r->alive = 0;
}

const char *p1_link_name(int l)
{
    switch (l) {
    case P1_LINK_IDLE: return "idle";
    case P1_LINK_LIVE: return "live";
    case P1_LINK_STALLED: return "stalled";
    case P1_LINK_LOST: return "LOST";
    default: return "?";
    }
}

const char *p1_mode_name(int m)
{
    switch (m) {
    case P1_MODE_RUN: return "run";
    case P1_MODE_NEUTRAL: return "neutral";
    case P1_MODE_SILENT: return "silent";
    default: return "?";
    }
}

void p1_rx_status(const p1_rx *r, char *out, size_t size)
{
    if (!size) return;
    snprintf(out, size,
             "rx: link %s, pad %s, hold %04x, epoch %u | packets %u records %u repeat %u late %u lost %u wrong-epoch %u | pushed %u rejected %u neutral %u held-dropped %u | stalls %u lost-link %u resumes %u | "
             "latency +%.0f ms (max +%u) | position: samples %u mean %.1f max %.1f, tp %u (resume %u flag %u jump %u hard %u soft %u), soft ticks %u",
             p1_link_name(r->link), p1_mode_name(r->mode), r->hold_flags, r->epoch, r->st.packets, r->st.records, r->st.keepalive_or_dup, r->st.late, r->st.lost, r->st.wrong_epoch, r->st.pushed, r->st.rejected,
             r->st.neutral_pushes, r->st.held_dropped, r->st.stalls, r->st.losts, r->st.resumes, (double)r->lat_ewma, r->lat_max, r->st.corr_samples,
             r->st.corr_samples ? (double)(r->st.err_sum / (float)r->st.corr_samples) : 0.0, (double)r->st.err_max, r->st.tp_total, r->st.tp_resume, r->st.tp_flag, r->st.tp_jump, r->st.tp_hard, r->st.tp_soft,
             r->st.soft_ticks);
}

void p1_tx_status(const p1_tx *t, char *out, size_t size)
{
    if (!size) return;
    snprintf(out, size, "tx: packets %u records %u keepalives %u, seq %u, redundancy %d, keepalive %d ms, stick eps %d", t->packets, t->records, t->keepalives, t->seq, t->cfg.redundancy, t->cfg.keepalive_ms,
             t->cfg.stick_eps);
}

/* ================================================================== position correction */
void p1_corr_defaults(p1_corr_cfg *c)
{
    c->soft_xz = 40.0f, c->hard_xz = 200.0f, c->hard_y = 120.0f, c->jump_xz = 500.0f, c->soft_ms = 1500, c->min_tp_ms = 750;
}

int p1_correct(p1_rx *r, uint32_t now, const p1_corr_cfg *cfgp, const p1_unit_ops *ops, p1_corr_result *res)
{
    p1_corr_cfg cfg;
    if (cfgp) cfg = *cfgp;
    else p1_corr_defaults(&cfg);
    memset(res, 0, sizeof *res);
    res->action = P1_C_NONE, res->reason = P1_R_NONE;
    int wanted = r->need_tp || r->tele_pending;
    int waitres = wanted ? P1_C_WAIT : P1_C_NONE;
    if (!ops || !ops->get) return res->action = waitres;
    if (r->mode != P1_MODE_RUN || r->hold_flags) {
        r->soft_on = 0;
        return res->action = waitres;
    }
    if (!r->tgt.valid || tdiff(now, r->state_rx) > r->cfg.state_stale_ms || (r->tgt.flags & P1_HOLD_MASK) || r->tgt.epoch != r->epoch) return res->action = waitres;
    if (fabsf(r->tgt.pos[0]) < 0.01f && fabsf(r->tgt.pos[1]) < 0.01f && fabsf(r->tgt.pos[2]) < 0.01f) return res->action = waitres;   /* (0,0,0): units not placed (loading) */
    float pos[3], yaw = 0;
    if (!ops->get(ops->user, pos, &yaw)) return res->action = waitres;
    if (fabsf(pos[0]) < 0.01f && fabsf(pos[1]) < 0.01f && fabsf(pos[2]) < 0.01f) return res->action = waitres;
    float dx = r->tgt.pos[0] - pos[0], dz = r->tgt.pos[2] - pos[2];
    float exz = sqrtf(dx * dx + dz * dz), ey = fabsf(r->tgt.pos[1] - pos[1]);
    res->err_xz = exz, res->err_y = ey;
    if (exz > 0.001f) res->dir[0] = dx / exz, res->dir[1] = dz / exz;
    r->st.corr_samples++;
    r->st.err_sum += exz;
    if (exz > r->st.err_max) r->st.err_max = exz;

    int reason = P1_R_NONE, limit = cfg.min_tp_ms;
    if (r->tele_pending) reason = P1_R_FLAG, limit = 250;
    else if (r->need_tp) {
        if (exz < cfg.soft_xz * 0.5f && ey < cfg.hard_y * 0.5f) {   /* already there: no jump, no animation reset */
            r->need_tp = 0;
        } else
            reason = P1_R_RESUME, limit = 250;
    }
    if (reason == P1_R_NONE) {
        if (exz >= cfg.jump_xz) reason = P1_R_JUMP, limit = 250;
        else if (exz >= cfg.hard_xz || ey >= cfg.hard_y) reason = P1_R_HARD;
        else if (exz >= cfg.soft_xz) {
            if (!r->soft_on) r->soft_on = 1, r->soft_since = now;
            if (tdiff(now, r->soft_since) >= cfg.soft_ms) reason = P1_R_SOFT_TIMEOUT;
            else {
                res->strength = (exz - cfg.soft_xz) / (cfg.hard_xz - cfg.soft_xz);
                if (res->strength > 1.0f) res->strength = 1.0f;
                r->st.soft_ticks++;
                return res->action = P1_C_SOFT;
            }
        } else
            r->soft_on = 0;
    }
    if (reason == P1_R_NONE) return res->action = P1_C_NONE;
    if (r->have_last_tp && tdiff(now, r->last_tp) < limit) return res->action = P1_C_WAIT;
    if (!ops->tp || !ops->tp(ops->user, r->tgt.pos, r->tgt.yaw)) return res->action = P1_C_WAIT;
    r->last_tp = now, r->have_last_tp = 1;
    r->need_tp = 0, r->tele_pending = 0, r->soft_on = 0;
    r->st.tp_total++;
    switch (reason) {
    case P1_R_RESUME: r->st.tp_resume++; break;
    case P1_R_FLAG: r->st.tp_flag++; break;
    case P1_R_JUMP: r->st.tp_jump++; break;
    case P1_R_HARD: r->st.tp_hard++; break;
    default: r->st.tp_soft++; break;
    }
    res->reason = reason;
    return res->action = P1_C_TP;
}

/* ================================================================== STATE of this PC's character */
int p1_state_fill(cp_state *st, const p1_snapshot *s, uint16_t scene_flags, int rt, const float *prev_pos)
{
    memset(st, 0, sizeof *st);
    if (!s->valid) return 0;
    st->pos[0] = s->pos[0], st->pos[1] = s->pos[1], st->pos[2] = s->pos[2], st->yaw = s->yaw;
    st->action = (s->upper_id & 0xFFu) | ((s->mode & 0xFFu) << 8);
    if (st->action == 0x80000000u) st->action = 0;
    float hp = s->hp < 0 ? 0 : s->hp > 32000.0f ? 32000.0f : s->hp, hm = s->hpmax < 0 ? 0 : s->hpmax > 32000.0f ? 32000.0f : s->hpmax;
    st->hp = (int16_t)(hp + 0.5f), st->hpmax = (int16_t)(hm + 0.5f);
    st->weapon = (uint8_t)(s->weapon < 0 ? 0xF : s->weapon);
    st->pstate = s->pstate, st->ctrl = s->ctrl;
    uint16_t f = (uint16_t)(scene_flags & (CP_SF_LOADING | CP_SF_CUTSCENE | CP_SF_PAUSED | CP_SF_DOWN));   /* DOWN: the downed rule (downed1.c network mode) */
    if (s->aiming) f |= CP_SF_AIMING;
    if (rt > 128) f |= CP_SF_FIRING;
    if ((s->unit_flags & 1u) || s->hp <= 0.0f) f |= CP_SF_DEAD;
    if (prev_pos) {
        float dx = s->pos[0] - prev_pos[0], dz = s->pos[2] - prev_pos[2];
        if (dx * dx + dz * dz > 500.0f * 500.0f) f |= CP_SF_TELEPORT;
    }
    st->flags = f;
    return 1;
}

int p1_keep0_want(int setting, int solo, int real_present) { return setting == 1 || (setting < 0 && (solo || !real_present)); }
