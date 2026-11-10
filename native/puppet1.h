/*
 * puppet1.h - the logic of the remote human ("puppet") of the Revelations 1 network co-op (B5). Pure C, no Windows, no game memory, no session: the proxy (net1.c glue) feeds it
 * packets and samples and gives it the three interfaces it needs (a sink for the pad records = capture_push_remote, unit field access, a clock), the offline tests
 * (tests/test_puppet1.c) feed it simulated lossy links. See notes\net-puppet-re.md (variant A) and notes\net-puppet-impl.md.
 *
 * Idea: the other PC's human is a second human (kind 1) of THIS game, driven by the sender's pad record through capture_push_remote. The sender dedups its pad (a record is numbered
 * only when the pad changed), sends the newest record plus up to three older ones in every CP_PAD packet (loss cover without retransmission) and repeats the newest one as a keepalive.
 * The receiver applies each record once, in order (late / repeated ones are counted and dropped), neutralises the pad while the link stalls or the sender is in a state where its input
 * must not move the puppet (pause, loading, dead, cutscene), and corrects the POSITION against the STATE samples with a teleport (the engine is not deterministic, the pad alone drifts).
 *
 * Threading: everything here runs on the game's logic thread (net1_frame); the network thread only fills the session's pad ring.
 */
#ifndef PUPPET1_H
#define PUPPET1_H

#include <stddef.h>
#include <stdint.h>

#include "coop_proto.h"

/* ================================================================== sender: dedup, numbering, redundancy, keepalive */
typedef struct {
    int keepalive_ms;       /* an unchanged pad is repeated this often (default 50 = 20 Hz: the receiver's neutral timeout is 400 ms) */
    int min_interval_ms;    /* a changed pad starts a new record at most this often (default 15: 60 Hz) */
    int stick_eps;          /* a stick that moved less than this (0..32767) from the last NUMBERED record is the same record (default 192, 0.6 %); buttons and triggers are exact */
    int redundancy;         /* records per packet, 1..CP_PAD_MAX_REC (default 3: two losses in a row are covered) */
} p1_tx_cfg;

typedef struct {
    p1_tx_cfg cfg;
    int n;                                  /* numbered records kept (newest first), at most CP_PAD_MAX_REC */
    uint16_t seq;                           /* sequence number of hist[0] */
    struct { cp_pad_rec r; uint32_t t; } hist[CP_PAD_MAX_REC];
    int have_send;
    uint32_t last_send;
    uint16_t last_flags;
    uint32_t records, packets, keepalives;  /* statistics */
} p1_tx;

void p1_tx_defaults(p1_tx_cfg *c);
void p1_tx_init(p1_tx *t, const p1_tx_cfg *c);    /* c NULL = defaults */
void p1_tx_restart(p1_tx *t);                     /* a new level instance: the history and the sequence start again (the receiver resets with the epoch) */
/* One call per logic frame with the sender's pad now. Returns 1 when `out` must be sent (pid and epoch are filled by cs_send_pad). */
int p1_tx_step(p1_tx *t, uint32_t now_ms, const cp_pad_rec *cur, uint16_t flags, cp_pad *out);

/* ================================================================== receiver */
#define P1_HOLD_MASK (CP_SF_DEAD | CP_SF_DOWN | CP_SF_LOADING | CP_SF_CUTSCENE | CP_SF_PAUSED | CP_SF_HELD)   /* HELD (ep1-solo E11): the sender's human is held by an enemy (grab1.h) */

typedef struct {
    int neutral_ms;         /* no packet for this long: neutral pad (default 400) */
    int deadman_ms;         /* no packet for this long: the partner is lost, the puppet stands until packets return (default 10000, the engine's own type 3 dead-man) */
    int hold_repeat_ms;     /* while held neutral, the neutral record is repeated this often (default 500) */
    int state_stale_ms;     /* a STATE sample older than this is not used for the position (default 600) */
} p1_rx_cfg;

enum { P1_LINK_IDLE = 0, P1_LINK_LIVE = 1, P1_LINK_STALLED = 2, P1_LINK_LOST = 3 };
enum { P1_MODE_RUN = 0, P1_MODE_NEUTRAL = 1 /* pad forced neutral, position still corrected */, P1_MODE_SILENT = 2 /* cutscene: the local engine drives the puppet; one neutral record on entry, then nothing is written */ };

/* where the records go: capture_push_remote(slot, rec, seq, ts) in the game. seq is the receiver's OWN output counter (strictly rising), so neutral records and
 * resumed records never collide with the sender's numbering. Returns 1 when accepted. */
typedef struct {
    int (*push)(void *user, const cp_pad_rec *rec, uint32_t seq, uint32_t ts_ms);
    void *user;
} p1_sink;

#define P1_PENDING 16

typedef struct {
    float pos[3], yaw;
    uint16_t flags;
    uint8_t epoch;
    int valid;
    uint32_t t_ms;          /* sender stamp of the sample (used to see a teleport flag once) */
} p1_target;

typedef struct {
    p1_rx_cfg cfg;
    p1_sink sink;
    /* sequence state */
    int have_seq, have_epoch;
    uint8_t epoch;
    uint32_t ext_hi;        /* highest extended sequence number seen */
    uint32_t applied_hi;    /* highest extended sequence number applied or dropped by a hold */
    uint32_t out_seq;       /* output counter for the sink */
    struct { cp_pad_rec r; uint32_t seq, t; } pend[P1_PENDING];
    int npend;
    cp_pad_rec latest;      /* newest record seen (resumed when a hold ends) */
    int have_latest;
    /* link / hold */
    uint32_t last_rx;       /* local time of the last accepted packet */
    int have_rx;
    int alive;              /* a packet arrived since the last tick (a keepalive refreshes the sink: the capture module's own timeout must not zero a held stick) */
    uint16_t pkt_flags;     /* flags of the newest packet */
    uint16_t state_flags;   /* flags of the newest STATE sample */
    uint32_t state_rx;      /* local time of the newest STATE sample */
    int link, mode;
    int held_seen;          /* a hold or a lost link was seen since the pad last ran: the return needs a teleport */
    uint16_t hold_flags;    /* the hold bits that decide now */
    uint32_t neutral_at;
    int need_tp;            /* a teleport to the sender's position is due as soon as a fresh sample and the unit exist (after a hold, the partner's return, a new level) */
    /* position target */
    p1_target tgt;
    uint32_t tele_stamp; int have_tele_stamp; int tele_pending;
    uint32_t soft_since; int soft_on;
    uint32_t last_tp; int have_last_tp;
    /* latency estimate (sender stamp against the local clock, relative to the best case seen) */
    int32_t off_min, off_win_min; uint32_t off_win_start; int have_off;
    float lat_ewma; uint32_t lat_max;
    /* statistics */
    struct {
        uint32_t packets, records, keepalive_or_dup, late, lost, wrong_epoch, pending_overflow, pushed, rejected, neutral_pushes, held_dropped;
        uint32_t stalls, losts, resumes;
        uint32_t tp_total, tp_resume, tp_flag, tp_jump, tp_hard, tp_soft, soft_ticks;
        uint32_t corr_samples; float err_sum, err_max;
    } st;
} p1_rx;

void p1_rx_defaults(p1_rx_cfg *c);
void p1_rx_init(p1_rx *r, const p1_rx_cfg *c, p1_sink sink);   /* c NULL = defaults */
/* A CP_PAD packet from the partner (already past the session's phase / epoch check). Stores the new records; they are applied by p1_rx_tick. cur_epoch = the session's epoch. */
void p1_rx_packet(p1_rx *r, uint32_t now_ms, const cp_pad *pkt, unsigned cur_epoch);
/* A STATE sample of the partner's character (from cs_remote_sample, or a raw state). valid = 0 clears the target (no sample / stale). */
void p1_rx_state(p1_rx *r, uint32_t now_ms, int valid, const float pos[3], float yaw, uint16_t flags, uint32_t t_ms, unsigned epoch);
/* Once per logic frame after the packets and the sample: link state, hold, applies the pending records through the sink. local_flags = CP_SF_* bits of THIS PC
 * (LOADING while the level loads, PAUSED, CUTSCENE) that also hold the puppet. */
void p1_rx_tick(p1_rx *r, uint32_t now_ms, unsigned cur_epoch, uint16_t local_flags);
/* Re-arm a teleport to the sender's position (e.g. after the roles were bound). */
void p1_rx_want_tp(p1_rx *r);
const char *p1_link_name(int link);
const char *p1_mode_name(int mode);
void p1_rx_status(const p1_rx *r, char *out, size_t size);
void p1_tx_status(const p1_tx *t, char *out, size_t size);

/* ================================================================== position correction (unit field access behind an interface) */
typedef struct {
    void *user;
    int (*get)(void *user, float pos[3], float *yaw);               /* the puppet unit's position (+0x40..) and heading (+0x1180); 0 = the unit does not exist now */
    int (*tp)(void *user, const float pos[3], float yaw);          /* one hard move, as the console `tp` of probe.c (teleport byte, position, +0x4C = 0, 0x6BEF50) plus the heading; 1 = done */
} p1_unit_ops;

typedef struct {
    float soft_xz;          /* below: nothing (default 40) */
    float hard_xz;          /* from here: teleport (default 200) */
    float hard_y;           /* vertical error that teleports (default 120) */
    float jump_xz;          /* from here: teleport at once, even inside the rate limit (default 500) */
    int soft_ms;            /* an error between soft and hard that lasts this long is teleported (default 1500) */
    int min_tp_ms;          /* at most one teleport per this (default 750), jumps and resumes excepted (250) */
} p1_corr_cfg;

enum { P1_C_NONE = 0, P1_C_SOFT = 1 /* inside the soft band: the caller may nudge (dir / strength), nothing was written */, P1_C_TP = 2, P1_C_WAIT = 3 /* a correction is wanted but not possible now (no unit / no fresh sample / hold) */ };
enum { P1_R_NONE = 0, P1_R_RESUME = 1, P1_R_FLAG = 2, P1_R_JUMP = 3, P1_R_HARD = 4, P1_R_SOFT_TIMEOUT = 5 };

typedef struct {
    int action, reason;
    float err_xz, err_y;    /* puppet against target (target - puppet in dir) */
    float dir[2];           /* unit vector (x, z) from the puppet to the target */
    float strength;         /* 0..1 inside the soft band */
} p1_corr_result;

void p1_corr_defaults(p1_corr_cfg *c);
/* Once per logic frame after p1_rx_tick. Axes: x and z are the ground plane, y is up (RE engine). */
int p1_correct(p1_rx *r, uint32_t now_ms, const p1_corr_cfg *cfg, const p1_unit_ops *ops, p1_corr_result *res);

/* ================================================================== STATE of this PC's character */
typedef struct {
    float pos[3], yaw;      /* unit +0x40.. , +0x1180 */
    float hp, hpmax;        /* +0xE38, +0xE40 */
    uint32_t upper_id;      /* upper-body state id (machine [+4]+0x18) */
    uint32_t mode;          /* +0xF08 */
    uint32_t pstate;        /* +0xF3C */
    uint32_t ctrl;          /* +0xF44 */
    uint32_t unit_flags;    /* +0xE1C: bit 0 = dead */
    int weapon;             /* bag slot +0xD4, 0xF = none */
    int aiming;             /* byte +0x1151 */
    int valid;              /* the unit exists */
} p1_snapshot;

/* Fill the STATE of this PC's character: Rev1 reading of the fields (coop_proto.h). scene_flags = CP_SF_* of the PC (cutscene, pause, loading); rt = right trigger of the
 * local pad (FIRING); prev_pos (may be NULL) lets a jump of more than 500 units set CP_SF_TELEPORT. Returns 0 when there is nothing to send (no unit). */
int p1_state_fill(cp_state *st, const p1_snapshot *s, uint16_t scene_flags, int rt, const float *prev_pos);

/* round 2: keep0 (a neutral stand-in pad on XInput user 0 so that Pad[1] gets its turn): setting -1 automatic / 0 never / 1 always; automatic = solo, or no
 * REAL gamepad on user 0 (re-checked while the roles are bound: a pad plugged in after the bind must not be masked by the stand-in). 1 = the stand-in should be on */
int p1_keep0_want(int setting, int solo, int real_present);

#endif
