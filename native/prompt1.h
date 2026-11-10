/*
 * prompt1.h - E8 (ep1-probe): a story prompt pressed by one human counts on both PCs. Pure, no game.
 * Evidence: notes\ep1-probe.md sections 1 and 5 (static: build 13298487).
 *
 * The engine: a story prompt is a PCS action (`PcsStartACTriggerButton` 0x76E310) of a cutscene script: its FSM waits for mPcsActionStatus[k] == 2
 * (getter 0x75D390 = the action command's status, record + 0x9E0 + k * 0x150 + 0xC). The action command manager (sActionCommand_vf6 0x412600) reads the
 * pad of the current slot Pad[[sGamePad + 0x870]] only, so on each PC only the LOCAL human's press counts. The prompt itself appears on a PC when any unit
 * stands in its zone (FUN_0074BBF0: Result = any hit), i.e. also for the partner's puppet. Each PC runs its own copy of the script (e1s2 live: 11_10b_0
 * started on both PCs, ended differently). So with two humans the press of one human leaves the other PC's copy waiting: its script never ends there (the
 * cutscene-end barrier then waits 60 s), and the FSM behind it (the grating's Ooze movie 11_088_0, the 1-3 tied man 13_020_0 ...) stops on that PC.
 * The rule: each PC watches the status of the six action commands of its 16 PCS slots; a rise to 2 that this PC did not make itself is announced as
 * (script hash, command index); the other PC, when the same script runs in one of its slots, writes status 2 into that command (the FSM reads the status
 * directly, and the manager keeps a status until the next pad edge). A press that arrives before the local copy of the prompt is kept PR1_KEEP_MS.
 * No echo: a status this PC forced is not announced. No double press: a remote press of a prompt this PC pressed itself within PR1_QUIET_MS is dropped.
 * Everything belongs to one level instance (pr1_reset at an epoch change).
 */
#ifndef PROMPT1_H
#define PROMPT1_H
#include <stdint.h>
#include <string.h>

#define PR1_SLOTS 16               /* sPcsManager: 16 script slots */
#define PR1_CMDS 6                 /* cFsmActionPcs: 6 action commands (ctor 0x778CB0) */
#define PR1_PRESSED 2u             /* the status the prompt FSMs wait for */
#define PR1_KEEP_MS 8000u          /* a remote press waits this long for the local copy of its script */
#define PR1_QUIET_MS 3000u         /* echo / double-press window */
#define PR1_MAXPEND 8
#define PR1_MAXRECENT 16

typedef struct { uint32_t hash; uint8_t st[PR1_CMDS]; uint8_t act[PR1_CMDS]; } pr1_view;   /* one slot as the glue read it: hash 0 = empty; act = the command is registered (shown) */
typedef struct { uint32_t hash; uint8_t cmd; uint32_t at; } pr1_ev;
typedef struct {
    uint32_t hash[PR1_SLOTS];
    uint8_t st[PR1_SLOTS][PR1_CMDS];
    pr1_ev pend[PR1_MAXPEND];      /* remote presses waiting for the local copy */
    unsigned npend;
    pr1_ev own[PR1_MAXRECENT];     /* presses of this PC's own human (no remote press of the same prompt is forced on top) */
    unsigned nown;
    pr1_ev forced[PR1_MAXRECENT];  /* presses this PC forced (never announced back) */
    unsigned nforced;
    unsigned announced, echo_dropped, rx, applied, expired, dup_dropped, own_dropped;
} pr1_t;

static inline void pr1_reset(pr1_t *p)
{
    unsigned a = p->announced, e = p->echo_dropped, r = p->rx, ap = p->applied, ex = p->expired, d = p->dup_dropped, o = p->own_dropped;
    memset(p, 0, sizeof *p);
    p->announced = a, p->echo_dropped = e, p->rx = r, p->applied = ap, p->expired = ex, p->dup_dropped = d, p->own_dropped = o;
}

static inline int pr1__recent(const pr1_ev *v, unsigned n, uint32_t hash, unsigned cmd, uint32_t now)
{
    for (unsigned i = 0; i < n; i++)
        if (v[i].hash == hash && v[i].cmd == cmd && (uint32_t)(now - v[i].at) < PR1_QUIET_MS) return 1;
    return 0;
}
static inline void pr1__push(pr1_ev *v, unsigned *n, unsigned cap, uint32_t hash, unsigned cmd, uint32_t now)
{
    unsigned k = 0, oldest = 0;
    for (; k < *n; k++)
        if (v[k].hash == hash && v[k].cmd == cmd) break;
    if (k == *n) {
        if (*n < cap) (*n)++;
        else {
            for (unsigned i = 1; i < cap; i++) if ((int32_t)(v[i].at - v[oldest].at) < 0) oldest = i;
            k = oldest;
        }
    }
    v[k].hash = hash, v[k].cmd = (uint8_t)cmd, v[k].at = now;
}

/* the scan of one pass: view[] = the 16 slots now. Returns the number of presses to announce (written to out[], at most cap): rises to PR1_PRESSED of a
 * command in a slot that runs the same script as on the last pass, not forced here within PR1_QUIET_MS. */
static inline unsigned pr1_scan(pr1_t *p, const pr1_view *view, uint32_t now, pr1_ev *out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned s = 0; s < PR1_SLOTS; s++) {
        const pr1_view *v = &view[s];
        int same = v->hash && v->hash == p->hash[s];
        for (unsigned c = 0; c < PR1_CMDS; c++) {
            int rose = same && v->st[c] == PR1_PRESSED && p->st[s][c] != PR1_PRESSED;
            if (rose) {
                if (pr1__recent(p->forced, p->nforced, v->hash, c, now)) p->echo_dropped++;
                else {
                    pr1__push(p->own, &p->nown, PR1_MAXRECENT, v->hash, c, now);
                    if (n < cap) out[n].hash = v->hash, out[n].cmd = (uint8_t)c, out[n].at = now, n++;
                    p->announced++;
                }
            }
            p->st[s][c] = v->st[c];
        }
        p->hash[s] = v->hash;
    }
    return n;
}

/* a press announced by the other PC. 1 = kept (pr1_due hands it out), 0 = dropped (this PC's own human pressed the same prompt just now, or a duplicate) */
static inline int pr1_remote(pr1_t *p, uint32_t hash, unsigned cmd, uint32_t now)
{
    p->rx++;
    if (!hash || cmd >= PR1_CMDS) return 0;
    if (pr1__recent(p->own, p->nown, hash, cmd, now)) { p->own_dropped++; return 0; }
    for (unsigned i = 0; i < p->npend; i++)
        if (p->pend[i].hash == hash && p->pend[i].cmd == cmd) { p->dup_dropped++; return 0; }
    if (p->npend >= PR1_MAXPEND) { p->pend[0] = p->pend[--p->npend]; p->expired++; }
    p->pend[p->npend].hash = hash, p->pend[p->npend].cmd = (uint8_t)cmd, p->pend[p->npend].at = now, p->npend++;
    return 1;
}

/* the next press to force now: a kept remote press whose script runs in a slot of view[] with that command shown (registered) and not pressed. 1 = *slot /
 * *cmd filled (the glue writes the status, then calls pr1_forced); expired presses are dropped on the way */
static inline int pr1_due(pr1_t *p, const pr1_view *view, uint32_t now, unsigned *slot, unsigned *cmd)
{
    for (unsigned i = 0; i < p->npend;) {
        pr1_ev *e = &p->pend[i];
        if ((uint32_t)(now - e->at) > PR1_KEEP_MS) { p->pend[i] = p->pend[--p->npend]; p->expired++; continue; }
        for (unsigned s = 0; s < PR1_SLOTS; s++) {
            if (view[s].hash != e->hash || !view[s].act[e->cmd] || view[s].st[e->cmd] == PR1_PRESSED) continue;
            *slot = s, *cmd = e->cmd;
            return 1;
        }
        i++;
    }
    return 0;
}

/* the glue forced (slot, cmd) of this script: the pending press is used, the rise it makes is not announced */
static inline void pr1_forced(pr1_t *p, uint32_t hash, unsigned cmd, uint32_t now)
{
    for (unsigned i = 0; i < p->npend; i++)
        if (p->pend[i].hash == hash && p->pend[i].cmd == cmd) { p->pend[i] = p->pend[--p->npend]; break; }
    pr1__push(p->forced, &p->nforced, PR1_MAXRECENT, hash, cmd, now);
    p->applied++;
}

/* the key of a script: FNV-1a 32 of its resource path, case-folded ('\\' and '/' equal), never 0 */
static inline uint32_t pr1_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c == '/') c = '\\';
        h = (h ^ c) * 16777619u;
    }
    return h ? h : 1u;
}

/* wire body of the ext event (world1 reliable lane, sub CP_W1_PROMPT): hash u32, command u8 */
static inline unsigned pr1_enc(uint8_t *o, unsigned cap, uint32_t hash, unsigned cmd)
{
    if (cap < 5 || cmd >= PR1_CMDS) return 0;
    o[0] = (uint8_t)hash, o[1] = (uint8_t)(hash >> 8), o[2] = (uint8_t)(hash >> 16), o[3] = (uint8_t)(hash >> 24), o[4] = (uint8_t)cmd;
    return 5;
}
static inline int pr1_dec(const uint8_t *b, unsigned n, uint32_t *hash, unsigned *cmd)
{
    if (n != 5 || b[4] >= PR1_CMDS) return 0;
    *hash = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24, *cmd = b[4];
    return 1;
}

#endif
