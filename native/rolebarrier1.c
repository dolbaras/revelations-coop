/*
 * rolebarrier1.c - see rolebarrier1.h. Pure logic: roles, the five engine detours (byte checks, stub code), the rendezvous / load barrier, the Retry / Continue triple.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rolebarrier1.h"

#define PHASE_LOADING 2   /* enum cp_phase in coop_proto.h (this file stays free of it; the test checks the numbers) */
#define PHASE_PLAYING 3
#define TRIPLE_FRESH_MS 30000u
#define RB_RESTART_DUP_MS 10000u   /* continue1: a restart call this soon after one with the host's triple is the same Continue */
#define RB_LS_STALE_MS 600000u     /* continue1: an armed level start older than this is not held */
#define RB_CR_ANSWER_MS 10000u     /* continue1: the host answers the guest's Continue request within this time or drops it */
/* continue1: this PC's saved checkpoint is the host's (the same mID in the same section and checkpoints) */
#define RB_AREA_NONE 0x91FF0000u /* a cleared record (sCheckPoint::cData_vf7) */
static int cp_same(const rb_cprec *x, const rb_cprec *h) { return x->have && h->have && x->id == h->id && x->area == h->area && x->a == h->a && x->b == h->b; }

/* ================================================================ 1. role */
int rb_role_own_req(int role) { return role == 1 ? 1 : -1; }

void rb_role_init(rb_role_t *r) { memset(r, 0, sizeof *r); }

int rb_role_step(rb_role_t *r, int started, int role, rb_role_act *out)
{
    memset(out, 0, sizeof *out);
    out->act = RBR_NONE, out->prev = -1;
    if (!started || (role != 0 && role != 1)) {
        if (r->applied) {
            out->act = RBR_RELEASE, out->role = r->role;
            r->applied = 0, r->releases++;
        }
        return out->act;
    }
    if (!r->applied || r->role != role) {
        out->act = RBR_APPLY, out->role = role, out->own_req = rb_role_own_req(role), out->prev = r->applied ? r->role : -1;
        r->applied = 1, r->role = role, r->applies++;
    }
    return out->act;
}

/* ================================================================ 2. hooks */
/* All the stolen bytes are whole instructions without relative operands (checked against rerev.exe with a disassembler, notes\net-role-barrier.md). */
const rbk_def rbk_defs[RBK_N] = {
    {"epend", "cEpisodeEndState_vf7", 0x42D160u, {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C}, 6, RBK_PRE, 4},
    {"pcs", "FUN_008916E0 sPcsManager start (FsmStartPcsDemo)", 0x8916E0u, {0x83, 0xEC, 0x08, 0x57, 0x8B, 0xF9}, 6, RBK_PRE, -1},
    {"movie", "FUN_00785580 sFsmManager play movie (FsmPlayDemoMovie)", 0x785580u, {0x83, 0xEC, 0x54, 0x53, 0x33, 0xDB}, 6, RBK_PRE, -1},
    {"pcsend", "sPcsManager mPcsIsEnd getter 0x88FC20", 0x88FC20u, {0x8B, 0x41, 0x28, 0x83, 0xF8, 0x10}, 6, RBK_POST, -1},
    {"restart", "FUN_00720870 restart at a checkpoint (Retry / Continue)", 0x720870u, {0x51, 0x56, 0x8B, 0xF1, 0x89, 0x74, 0x24, 0x04}, 8, RBK_PRE, 0xC},
    /* push ebx; mov ebx,ecx; push esi; test ebx,ebx (the je that follows is relative: not taken over) */
    {"start", "FUN_00720540 start record from the table (title New Game / Continue / Load, checkpoint reload, section change)", 0x720540u, {0x53, 0x8B, 0xD9, 0x56, 0x85, 0xDB}, 6, RBK_PRE, -1},
    /* continue1: push ecx; push esi; push edi; mov edi,ecx; xor esi,esi (fastcall, no stack arguments: a POST filter of its al) */
    {"lstart", "FUN_0041CFC0 the aCut area's load step (1 = the level may start: units placed, scene scripts, enemies next)", 0x41CFC0u, {0x51, 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xF6}, 7, RBK_POST, -1},
    /* mov eax,[0x1169110]; test eax,eax (cdecl, one byte argument, plain ret: an early return is a plain ret) */
    {"cont", "FUN_0041EFE0 the stock Continue request (game-over Yes, the pause menu's checkpoint reload: area change to aStartContinue)", 0x41EFE0u, {0xA1, 0x10, 0x91, 0x16, 0x01, 0x85, 0xC0}, 7, RBK_PRE, 0},
};

const char *rbk_state_name(int st)
{
    switch (st) {
    case RBK_OFF: return "off";
    case RBK_ON: return "ON";
    case RBK_MISMATCH: return "REFUSED(bytes)";
    case RBK_ERROR: return "ERROR";
    default: return "?";
    }
}

void rbk_jmp_build(uint8_t out[5], uint32_t from, uint32_t to)
{
    out[0] = 0xE9;
    uint32_t rel = to - (from + 5);
    memcpy(out + 1, &rel, 4);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* PRE stub (the callback gets a pointer to the saved registers + stack; its return value != 0 = return early when the definition has an early return):
 *    push 0 / pushfd / pushad / mov eax,esp / push eax / mov eax,CB / call eax / add esp,4 / mov [esp+24h],eax / popad / popfd
 *    cmp dword [esp],0 / lea esp,[esp+4] / jnz hold        (lea keeps the flags)
 *    <stolen bytes> / jmp va+steal
 * hold: xor eax,eax / ret N
 * POST stub (a function with no stack arguments; the callback gets `this` and the FULL eax, returns the eax to hand on; ecx / edx as the function left them):
 *    mov [cell],ecx / call tramp / push ecx / push edx / push eax / push [cell] / mov eax,CB / call eax / add esp,8 / pop edx / pop ecx / ret
 * tramp: <stolen bytes> / jmp va+steal
 * (live 2026-11-06: the first version handed on `movzx eax,al`; the getter 0x88FC20 answers (handle & ~0xFF) | 1, e.g. 0xFFFFFF01, when its handle [mgr+0x28] is >= 16 (-1 = none), so the upper bits
 *  changed although no rendezvous ran. Now nothing changes unless the callback filters.) */
int rbk_stub_build(uint8_t *out, size_t cap, const rbk_def *d, uint32_t stub_addr, uint32_t cb, uint32_t cell)
{
    uint8_t *p = out;
    if (cap < 96 || d->steal < 5 || d->steal > RBK_SIG_MAX) return 0;
    if (d->kind == RBK_PRE) {
        *p++ = 0x6A, *p++ = 0x00;                                   /* push 0 */
        *p++ = 0x9C;                                                /* pushfd */
        *p++ = 0x60;                                                /* pushad */
        *p++ = 0x89, *p++ = 0xE0;                                   /* mov eax,esp */
        *p++ = 0x50;                                                /* push eax */
        *p++ = 0xB8, put32(p, cb), p += 4;                          /* mov eax,cb */
        *p++ = 0xFF, *p++ = 0xD0;                                   /* call eax */
        *p++ = 0x83, *p++ = 0xC4, *p++ = 0x04;                      /* add esp,4 */
        *p++ = 0x89, *p++ = 0x44, *p++ = 0x24, *p++ = 0x24;         /* mov [esp+24h],eax */
        *p++ = 0x61;                                                /* popad */
        *p++ = 0x9D;                                                /* popfd */
        *p++ = 0x83, *p++ = 0x3C, *p++ = 0x24, *p++ = 0x00;         /* cmp dword ptr [esp],0 */
        *p++ = 0x8D, *p++ = 0x64, *p++ = 0x24, *p++ = 0x04;         /* lea esp,[esp+4] */
        uint8_t *jnz = p;
        *p++ = d->hold_ret >= 0 ? 0x75 : 0x90, *p++ = 0x90;         /* jnz hold (rel8 filled below) | two nops */
        memcpy(p, d->sig, (size_t)d->steal), p += d->steal;         /* the stolen instructions */
        rbk_jmp_build(p, stub_addr + (uint32_t)(p - out), d->va + (uint32_t)d->steal), p += 5;
        if (d->hold_ret >= 0) {
            jnz[1] = (uint8_t)(p - (jnz + 2));
            *p++ = 0x31, *p++ = 0xC0;                               /* xor eax,eax */
            if (d->hold_ret == 0) *p++ = 0xC3;
            else *p++ = 0xC2, *p++ = (uint8_t)(d->hold_ret & 0xFF), *p++ = (uint8_t)(d->hold_ret >> 8);
        }
        return (int)(p - out);
    }
    /* POST */
    *p++ = 0x89, *p++ = 0x0D, put32(p, cell), p += 4;               /* mov [cell],ecx */
    uint8_t *call = p;
    *p++ = 0xE8, p += 4;                                            /* call tramp (filled below) */
    *p++ = 0x51;                                                    /* push ecx */
    *p++ = 0x52;                                                    /* push edx */
    *p++ = 0x50;                                                    /* push eax (the whole result) */
    *p++ = 0xFF, *p++ = 0x35, put32(p, cell), p += 4;               /* push dword ptr [cell] */
    *p++ = 0xB8, put32(p, cb), p += 4;                              /* mov eax,cb */
    *p++ = 0xFF, *p++ = 0xD0;                                       /* call eax */
    *p++ = 0x83, *p++ = 0xC4, *p++ = 0x08;                          /* add esp,8 */
    *p++ = 0x5A;                                                    /* pop edx */
    *p++ = 0x59;                                                    /* pop ecx */
    *p++ = 0xC3;                                                    /* ret */
    uint32_t tramp = stub_addr + (uint32_t)(p - out);
    put32(call + 1, tramp - (stub_addr + (uint32_t)(call - out) + 5));
    memcpy(p, d->sig, (size_t)d->steal), p += d->steal;
    rbk_jmp_build(p, stub_addr + (uint32_t)(p - out), d->va + (uint32_t)d->steal), p += 5;
    return (int)(p - out);
}

#define BLOCK_BYTES 112u
#define CELL_OFF 104u

static void mlog(const rbk_mem *m, const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (m && m->log) m->log(m->user, line);
}

static void hexs(char *dst, size_t size, const uint8_t *b, int n)
{
    size_t o = 0;
    dst[0] = 0;
    for (int i = 0; i < n && o + 4 < size; i++) o += (size_t)snprintf(dst + o, size - o, "%s%02x", i ? " " : "", b[i]);
}

static int refuse(rbk_state *s, const rbk_def *d, const rbk_mem *m, int st, const char *fmt, ...)
{
    char why[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    s->st = st, s->refusals++;
    snprintf(s->why, sizeof s->why, "%s", why);
    mlog(m, "barrier: hook %s (0x%X) refused: %s", d->name, d->va, why);
    return 0;
}

int rbk_apply(rbk_state *s, const rbk_def *d, const rbk_mem *m, uint32_t callback)
{
    uint8_t cur[RBK_SIG_MAX];
    char hx[64];
    if (!m || !m->read || !m->write || !m->alloc) return refuse(s, d, m, RBK_ERROR, "no memory access");
    if (!m->read(m->user, d->va, cur, (uint32_t)d->steal)) return refuse(s, d, m, RBK_ERROR, "bytes at 0x%X cannot be read", d->va);
    if (s->st == RBK_ON && s->block) {
        uint8_t jmp[5];
        rbk_jmp_build(jmp, d->va, s->block);
        if (!memcmp(cur, jmp, 5)) return 1;   /* already in force */
        hexs(hx, sizeof hx, cur, d->steal);
        return refuse(s, d, m, RBK_MISMATCH, "our detour was overwritten (now %s)", hx);
    }
    if (memcmp(cur, d->sig, (size_t)d->steal)) {
        hexs(hx, sizeof hx, cur, d->steal);
        return refuse(s, d, m, RBK_MISMATCH, "bytes are %s, expected the original build's", hx);
    }
    if (!s->block) s->block = m->alloc(m->user, BLOCK_BYTES);
    if (!s->block) return refuse(s, d, m, RBK_ERROR, "no executable memory for the stub");
    uint8_t code[BLOCK_BYTES];
    memset(code, 0xCC, sizeof code);
    int n = rbk_stub_build(code, sizeof code, d, s->block, callback, s->block + CELL_OFF);
    if (n <= 0) return refuse(s, d, m, RBK_ERROR, "stub does not fit");
    s->cb = callback;
    if (!m->write(m->user, s->block, code, (uint32_t)n)) return refuse(s, d, m, RBK_ERROR, "cannot write the stub");
    uint8_t patch[RBK_SIG_MAX];
    memset(patch, 0x90, sizeof patch);
    rbk_jmp_build(patch, d->va, s->block);
    if (!m->write(m->user, d->va, patch, (uint32_t)d->steal)) return refuse(s, d, m, RBK_ERROR, "cannot write the detour");
    s->st = RBK_ON, s->applies++;
    s->why[0] = 0;
    mlog(m, "barrier: hook %s (%s) in force at 0x%X, stub 0x%X (%d bytes)", d->name, d->what, d->va, s->block, n);
    return 1;
}

int rbk_revert(rbk_state *s, const rbk_def *d, const rbk_mem *m)
{
    uint8_t cur[RBK_SIG_MAX], jmp[5];
    char hx[64];
    if (s->st != RBK_ON) return 1;
    if (!m->read(m->user, d->va, cur, (uint32_t)d->steal)) return refuse(s, d, m, RBK_ERROR, "bytes at 0x%X cannot be read", d->va);
    rbk_jmp_build(jmp, d->va, s->block);
    if (!memcmp(cur, d->sig, (size_t)d->steal)) {   /* somebody already took it out */
        s->st = RBK_OFF;
        return 1;
    }
    if (memcmp(cur, jmp, 5)) {
        hexs(hx, sizeof hx, cur, d->steal);
        return refuse(s, d, m, RBK_MISMATCH, "cannot revert, the bytes are foreign: %s", hx);
    }
    if (!m->write(m->user, d->va, d->sig, (uint32_t)d->steal)) return refuse(s, d, m, RBK_ERROR, "cannot write the original bytes back");
    s->st = RBK_OFF, s->reverts++;
    mlog(m, "barrier: hook %s taken out", d->name);
    return 1;
}

int rbk_probe(const rbk_def *d, const rbk_mem *m, uint32_t stub, char *out, size_t size)
{
    uint8_t cur[RBK_SIG_MAX], jmp[5];
    char hx[64];
    if (!m->read(m->user, d->va, cur, (uint32_t)d->steal)) {
        snprintf(out, size, "unreadable");
        return 0;
    }
    if (!memcmp(cur, d->sig, (size_t)d->steal)) {
        snprintf(out, size, "original");
        return 1;
    }
    if (stub) {
        rbk_jmp_build(jmp, d->va, stub);
        if (!memcmp(cur, jmp, 5)) {
            snprintf(out, size, "detoured (ours)");
            return 2;
        }
    }
    hexs(hx, sizeof hx, cur, d->steal);
    snprintf(out, size, "FOREIGN %s", hx);
    return 0;
}

/* ================================================================ 3. barrier */
uint32_t rb_hash(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s || !*s) return 0;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}

void rb_defaults(rb_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->section_ms = 180000, c->pcs_ms = 60000, c->latch_ms = 3000, c->settle_ms = 500, c->unplay_ms = 700, c->stale_epoch_ms = 8000, c->no_triple_hold_ms = 0, c->follow_ms = 1500;
    c->sec_follow_ms = 4000, c->sec_window_ms = 20000, c->sec_wait_ms = 30000, c->ep_close_cap_ms = 1800000u;
    c->xl_guard = 1;
    c->sec_stock = 1;
    c->cp_sync = 1, c->lstart_hold = 1, c->lstart_ms = 120000u, c->shield = 1, c->shield_ms = 300000u;
    c->follow_stock = 1, c->stock_wait_ms = 8000u, c->cont_req = 1, c->cont_wait_ms = 15000u, c->cont_dedup_ms = 10000u;
    c->cp_mirror = 1;
    c->title_load = 1, c->title_load_ms = 15000u;
}

void rb_init(rb_t *b, const rb_io *io, const rb_cfg *cfg)
{
    memset(b, 0, sizeof *b);
    b->io = *io;
    if (cfg) b->cfg = *cfg;
    else rb_defaults(&b->cfg);
    b->first_load = 1;
    b->tx_diff = b->rs_diff = b->applied_diff = -1;
}

static void blog(rb_t *b, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    snprintf(b->why, sizeof b->why, "%s", line);
    if (b->io.log) b->io.log(b->io.user, line);
}

void rb_reset(rb_t *b)
{
    memset(b->pt, 0, sizeof b->pt);
    memset(b->peer, 0, sizeof b->peer);
    memset(b->occ, 0, sizeof b->occ);
    b->peer_next = b->occ_next = 0;
    b->pcs_pending = 0, b->pcs_key = 0;
    b->play_prev = 0, b->play_since = b->unplay_since = 0;
    b->seen_playable = b->unplayed_in_epoch = 0;
    b->own_began = b->epoch_valid = b->epoch_first = 0;
    b->reported_valid = 0, b->reported_epoch = b->epoch_seen = 0, b->epoch_t0 = 0;
    b->first_load = 1;
    b->rs_have = 0, b->rs_wait_t0 = 0, b->rs_follow_seq = 0;
    b->sec_rx_seq = b->sec_follow_seq = 0, b->sec_follow_valid = 0;
    b->sec_stock_asked = 0, b->sec_stock_seq = 0, b->sec_stock_t = 0;
    b->st_pending = 0, b->tx_have = 0;
    b->rs_diff = b->applied_diff = -1;
    memset(b->nosess_logged, 0, sizeof b->nosess_logged);
    b->sec_pending = 0, b->sec_from = 0, b->sec_how = 0, b->own_sec_have = 0;
    b->ep_local = b->ep_peer = 0, b->ep_local_t = b->ep_peer_t = 0, b->ep_from = 0, b->ep_seen_flow = b->ep_closed = 0;
    b->lvl_area = 0, b->xl_epoch = 0;
    /* continue1 */
    b->rs_cp.have = b->applied_cp.have = b->tx_cp.have = 0;
    b->rs_last_have = 0, b->rs_last_t = 0;
    b->ls_armed = 0, b->sh_armed = 0, b->sh_loading = 0;
    b->fol_try_stock = b->fol_stock = b->fol_direct_done = 0;
    b->cont_t = 0, b->cont_ours = 0, b->cr_pending = 0;
    b->held = 0, b->held_fallback = 0;
    memset(&b->cpm_last, 0, sizeof b->cpm_last), memset(&b->cpm_mark, 0, sizeof b->cpm_mark), memset(&b->cpm_in, 0, sizeof b->cpm_in);
    b->tx_kind_once = 0, b->rs_kind = RB_KIND_UNKNOWN, b->tl_state = RB_TLS_IDLE, b->tl_t = 0;   /* rev1 live 15 */
}

void rb_set_enabled(rb_t *b, int on)
{
    on = on != 0;
    if (b->enabled == on) return;
    b->enabled = on;
    if (!on) rb_reset(b);
}

/* ---- messages ---- */
int rb_msg_arrive(uint8_t *out, size_t cap, int kind, uint32_t key, uint16_t n)
{
    if (cap < 9 || kind < 0 || kind >= RB_PT_N) return 0;
    out[0] = RB_MSG_VER, out[1] = RBM_ARRIVE, out[2] = (uint8_t)kind;
    memcpy(out + 3, &key, 4);
    memcpy(out + 7, &n, 2);
    return 9;
}

int rb_msg_restart_d(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b, int diff)
{
    int with = diff >= 0 && diff <= RB_DIFF_MAX;
    if (cap < (size_t)(with ? 17 : 16)) return 0;
    out[0] = RB_MSG_VER, out[1] = RBM_RESTART;
    memcpy(out + 2, &seq, 2);
    memcpy(out + 4, &area, 4);
    memcpy(out + 8, &a, 4);
    memcpy(out + 12, &b, 4);
    if (!with) return 16;
    out[16] = (uint8_t)diff;
    return 17;
}

int rb_msg_restart(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b) { return rb_msg_restart_d(out, cap, seq, area, a, b, -1); }

int rb_msg_restart_cp(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b, int diff, const rb_cp *cp)
{
    if (!cp || !cp->have) return rb_msg_restart_d(out, cap, seq, area, a, b, diff);
    if (cap < RB_RESTART_CP_LEN || rb_msg_restart_d(out, cap, seq, area, a, b, 0) != 17) return 0;
    int16_t id = (int16_t)(cp->id < -32768 ? -32768 : cp->id > 32767 ? 32767 : cp->id);
    uint16_t mask = (uint16_t)(cp->mask & 0xFFFFu);
    out[16] = diff >= 0 && diff <= RB_DIFF_MAX ? (uint8_t)diff : 0xFFu;
    memcpy(out + 17, &id, 2);
    memcpy(out + 19, &mask, 2);
    return RB_RESTART_CP_LEN;
}

int rb_msg_restart_k(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b, int diff, const rb_cp *cp, int kind)
{
    rb_cp none = {1, -1, 0};
    if (cap < RB_RESTART_K_LEN || rb_msg_restart_cp(out, cap, seq, area, a, b, diff, cp && cp->have ? cp : &none) != RB_RESTART_CP_LEN) return 0;
    out[21] = (uint8_t)((kind & 3) | (cp && cp->have ? 4 : 0));
    return RB_RESTART_K_LEN;
}

int rb_msg_contreq(uint8_t *out, size_t cap, uint16_t seq)
{
    if (cap < RB_CONTREQ_MSG_LEN) return 0;
    out[0] = RB_MSG_VER, out[1] = RBM_CONTREQ;
    memcpy(out + 2, &seq, 2);
    return RB_CONTREQ_MSG_LEN;
}

int rb_triple_ok(const rb_triple *t, char *why, size_t wsz)
{
    const char *w = NULL;
    if ((t->area >> 24) != 0x91u || (t->area & 0xFFu) != 0 || ((t->area >> 16) & 0xFFu) == 0xFFu || ((t->area >> 16) & 0xFFu) == 0) w = "area is not a campaign section id (0x91EESS00)";
    else if (t->a < -1 || t->a > 0xFFFF || t->b < -1 || t->b > 0xFFFF) w = "checkpoint id out of range";
    else if (t->a == 0 && t->b == 0) w = "both checkpoint ids are zero (the engine refuses that)";
    if (w && why && wsz) snprintf(why, wsz, "%s", w);
    return w == NULL;
}

static int peer_has(const rb_t *b, int kind, uint32_t key, uint16_t n)
{
    for (int i = 0; i < 32; i++)
        if (b->peer[i].used && b->peer[i].kind == kind && b->peer[i].key == key && b->peer[i].n == n) return 1;
    return 0;
}

int rb_on_event(rb_t *b, const uint8_t *body, unsigned len)
{
    if (!b->enabled) return 0;
    if (len < 2 || body[0] != RB_MSG_VER) {
        blog(b, "barrier: event ignored (version/length %u)", len);
        return 0;
    }
    if (body[1] == RBM_ARRIVE && len == 9 && body[2] < RB_PT_N) {
        uint32_t key;
        uint16_t n;
        memcpy(&key, body + 3, 4);
        memcpy(&n, body + 7, 2);
        b->arrivals_rx++;
        if (!peer_has(b, body[2], key, n)) {
            b->peer[b->peer_next] = (rb_arrival){body[2], key, n, 1};
            b->peer_next = (b->peer_next + 1) % 32;
        }
        return 1;
    }
    if (body[1] == RBM_RESTART && (len == 16 || len == 17 || len == RB_RESTART_CP_LEN || len == RB_RESTART_K_LEN)) {
        rb_triple t;
        uint16_t seq;
        char why[100];
        memcpy(&seq, body + 2, 2);
        memcpy(&t.area, body + 4, 4);
        memcpy(&t.a, body + 8, 4);
        memcpy(&t.b, body + 12, 4);
        if (b->host) {
            blog(b, "barrier: RESTART from the client ignored (the host decides)");
            return 0;
        }
        if (!rb_triple_ok(&t, why, sizeof why)) {
            b->rs_rejected++;
            blog(b, "barrier: RESTART rejected: %s (area %08X, %d, %d)", why, t.area, t.a, t.b);
            return 0;
        }
        if (b->ep_local || b->ep_peer) {   /* E5: after the end of Episode 1 nothing is followed (the next part is Episode 2) */
            b->rs_rejected++;
            blog(b, "barrier: RESTART #%u from the host (area %08X %d / %d) ignored: Episode 1 has ended", seq, t.area, t.a, t.b);
            return 0;
        }
        b->rs_have = 1, b->rs_seq = seq, b->rs_area = t.area, b->rs_a = t.a, b->rs_b = t.b, b->rs_t = b->io.now ? b->io.now(b->io.user) : 0;
        b->rs_diff = len >= 17 && body[16] <= RB_DIFF_MAX ? body[16] : -1;
        b->rs_cp.have = 0;
        b->rs_kind = len == RB_RESTART_K_LEN ? body[21] & 3 : RB_KIND_UNKNOWN;   /* rev1 live 15 */
        if (len == RB_RESTART_CP_LEN || (len == RB_RESTART_K_LEN && (body[21] & 4))) {   /* continue1: the host's checkpoint record */
            int16_t id;
            uint16_t mask;
            memcpy(&id, body + 17, 2);
            memcpy(&mask, body + 19, 2);
            b->rs_cp.have = 1, b->rs_cp.id = id, b->rs_cp.mask = mask, b->cp_got++;
            b->host_cp = (rb_cprec){1, id, mask, t.area, t.a, t.b};
        }
        b->rs_received++;
        b->sec_pending = 0;   /* a Continue of the host is newer than any section triple */
        if (b->rs_cp.have)
            blog(b, "barrier: RESTART #%u from the host: area %08X checkpoints %d / %d, difficulty %d, its checkpoint record: mID %d, passed mask %04X, start kind %s", seq, t.area, t.a, t.b,
                 b->rs_diff, b->rs_cp.id, b->rs_cp.mask, b->rs_kind == RB_KIND_SAVE ? "SAVE" : b->rs_kind == RB_KIND_NEW ? "NEW GAME" : "unknown");
        else
            blog(b, "barrier: RESTART #%u from the host: area %08X checkpoints %d / %d, difficulty %d, start kind %s", seq, t.area, t.a, t.b, b->rs_diff,
                 b->rs_kind == RB_KIND_SAVE ? "SAVE" : b->rs_kind == RB_KIND_NEW ? "NEW GAME" : "unknown");
        return 1;
    }
    if (body[1] == RBM_CPPASS && len == RB_CPPASS_MSG_LEN) {   /* continue1 task 4: the other PC passed a checkpoint */
        int16_t id, a, bb;
        uint16_t mask, seq;
        uint32_t area;
        memcpy(&id, body + 2, 2), memcpy(&mask, body + 4, 2), memcpy(&area, body + 6, 4), memcpy(&a, body + 10, 2), memcpy(&bb, body + 12, 2), memcpy(&seq, body + 14, 2);
        b->cpm_got++;
        b->cpm_in = (rb_cprec){1, id, mask, area, a, bb}, b->cpm_in_t = b->io.now ? b->io.now(b->io.user) : 0, b->cpm_try_t = 0;
        if (!b->host) b->host_cp = b->cpm_in;
        blog(b, "barrier: CPPASS #%u from the %s: it passed checkpoint mID %d (area %08X, %d / %d, mask %04X)", seq, b->host ? "guest" : "host", id, area, a, bb, mask);
        return 1;
    }
    if (body[1] == RBM_CONTREQ && len == RB_CONTREQ_MSG_LEN) {   /* continue1: the guest chose Continue */
        uint16_t seq;
        memcpy(&seq, body + 2, 2);
        if (!b->host) {
            blog(b, "barrier: CONTREQ from the host ignored (the guest asks, the host decides)");
            return 0;
        }
        b->cr_got++;
        b->cr_pending = 1, b->cr_t = b->io.now ? b->io.now(b->io.user) : 0;
        blog(b, "barrier: CONTREQ #%u from the guest: it chose Continue; this PC continues too (its own stock Continue, then RESTART)", seq);
        return 1;
    }
    if (body[1] == RBM_SECTION && len == RB_SECTION_MSG_LEN) {   /* E4: the host passed a section end; its next section */
        rb_triple t;
        uint16_t seq;
        uint32_t from;
        char why[100];
        memcpy(&seq, body + 2, 2);
        memcpy(&t.area, body + 4, 4);
        memcpy(&t.a, body + 8, 4);
        memcpy(&t.b, body + 12, 4);
        memcpy(&from, body + 16, 4);
        if (b->host) {
            blog(b, "barrier: SECTION from the client ignored (the host decides)");
            return 0;
        }
        if (!rb_triple_ok(&t, why, sizeof why) || !rb_ep1_area(t.area)) {
            b->sec_bad++;
            blog(b, "barrier: SECTION #%u from the host rejected: %s (area %08X, %d, %d)", seq, rb_ep1_area(t.area) ? why : "not a section of Episode 1", t.area, t.a, t.b);
            return 0;
        }
        if (b->ep_local || b->ep_peer) {
            b->sec_bad++;
            blog(b, "barrier: SECTION #%u from the host (area %08X) ignored: Episode 1 has ended", seq, t.area);
            return 0;
        }
        uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
        b->sec_got++;
        b->sec_from = from, b->sec_how = body[20];
        if (b->own_sec_have && b->own_sec_area == t.area && (int32_t)(now - b->own_sec_t) <= b->cfg.sec_window_ms) {
            b->sec_consumed++;
            blog(b, "barrier: SECTION #%u from the host: %08X -> %08X (%d / %d, host %s): this PC changed to that section %u ms ago by itself, nothing to follow", seq, from, t.area, t.a, t.b,
                 body[20] == RB_SEC_PEER ? "released by this PC's arrival" : body[20] == RB_SEC_TIMEOUT ? "released by its TIMEOUT" : "without a rendezvous",
                 (unsigned)(now - b->own_sec_t));
            return 1;
        }
        b->rs_have = 1, b->rs_area = t.area, b->rs_a = t.a, b->rs_b = t.b, b->rs_t = now, b->rs_diff = -1;   /* rs_seq stays the RESTART numbers' */
        b->rs_kind = RB_KIND_UNKNOWN;   /* rev1 live 15: a section change is no save start */
        b->sec_rx_seq = seq;
        b->rs_received++;
        b->sec_pending = 1;
        blog(b, "barrier: SECTION #%u from the host: it left %08X for %08X (start %d / %d, host %s): this PC follows unless it changes there by itself", seq, from, t.area, t.a, t.b,
             body[20] == RB_SEC_PEER ? "released by this PC's arrival" : body[20] == RB_SEC_TIMEOUT ? "released by its TIMEOUT" : "without a rendezvous");
        return 1;
    }
    if (body[1] == RBM_EPDONE && len == RB_EPDONE_MSG_LEN) {   /* E5: the other PC passed the end of Episode 1 */
        uint32_t from;
        memcpy(&from, body + 2, 4);
        b->ep_got++;
        if (!b->ep_peer) {
            b->ep_peer = 1, b->ep_peer_t = b->io.now ? b->io.now(b->io.user) : 0;
            if (!b->ep_from) b->ep_from = from;
        }
        if (!b->host && (b->rs_have || b->sec_pending)) blog(b, "barrier: a waiting host triple (area %08X) is dropped: Episode 1 has ended", b->rs_area);
        if (!b->host) b->rs_have = 0, b->sec_pending = 0;
        blog(b, "barrier: EPDONE from the %s: it passed the end of Episode 1 (left %08X)%s", b->host ? "client" : "host", from,
             b->ep_local ? "; this PC passed it too" : ": this PC has not (the session closes when the partner's end flow is over)");
        return 1;
    }
    b->rs_rejected += body[1] == RBM_RESTART;
    blog(b, "barrier: event ignored (sub %u, length %u)", body[1], len);
    return 0;
}

/* ---- the rendezvous ---- */
static uint16_t occurrence(rb_t *b, int kind, uint32_t key)
{
    for (int i = 0; i < 24; i++)
        if (b->occ[i].count && b->occ[i].kind == kind && b->occ[i].key == key) return ++b->occ[i].count;
    b->occ[b->occ_next] = (rb_occ){(uint8_t)kind, key, 1};
    b->occ_next = (b->occ_next + 1) % 24;
    return 1;
}

static const char *kind_name(int kind) { return kind == RB_SECTION ? "section end" : kind == RB_LSTART ? "level start" : "cutscene end"; }

int rb_gate(rb_t *b, int kind, uint32_t key)
{
    rb_sinfo si;
    uint8_t body[RB_MSG_MAX];
    if (kind < 0 || kind >= RB_PT_N) return RB_PROCEED;
    if (!b->enabled || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) {
        snprintf(b->why, sizeof b->why, "%s: no network session, goes on", kind_name(kind));
        b->k_nosession[kind]++;
        if (!b->nosess_logged[kind]) {   /* once until a gate with a session runs again: the reason is visible in the log, not only in `barrier status` */
            b->nosess_logged[kind] = 1;
            blog(b, "barrier: %s asked with no network session (enabled %d): goes on, nothing held", kind_name(kind), b->enabled);
        }
        return RB_PROCEED;
    }
    b->nosess_logged[kind] = 0;
    uint32_t now = b->io.now(b->io.user);
    rb_point *p = &b->pt[kind];
    /* not asked for a while: a new visit. Not for a cutscene end that is still pending (rb_pcs_begin starts the next one): its "ended" reads can be seconds
     * apart (e1p1 live: every 8.3 s), and a new occurrence per read never meets the partner's late arrival nor reaches the timeout (ep1-probe) */
    if (p->st != 0 && (uint32_t)(now - p->last) > b->cfg.latch_ms && (kind != RB_PCS || b->cfg.pcs_relatch)) p->st = 0;
    if (p->st == 0) {
        p->key = key, p->n = occurrence(b, kind, key), p->t0 = now, p->st = 1, p->held = 0;
        int len = rb_msg_arrive(body, sizeof body, kind, key, p->n);
        if (!len || !b->io.send || !b->io.send(b->io.user, body, (unsigned)len)) {
            p->st = 3;   /* cannot tell the other PC: do not wait for it */
            p->last = now;
            blog(b, "barrier: %s #%u key %08X: the arrival could not be sent, goes on (nothing held)", kind_name(kind), p->n, key);
            return RB_PROCEED;
        }
        b->arrivals_tx++;
        blog(b, "barrier: %s #%u key %08X reached, arrival sent", kind_name(kind), p->n, key);
    }
    p->last = now;
    if (p->st >= 2) return RB_PROCEED;
    if (peer_has(b, kind, p->key, p->n)) {
        p->st = 2, b->released_peer++, b->k_peer[kind]++;
        blog(b, "barrier: %s #%u %s by the other PC after %u ms (%u frames held)", kind_name(kind), p->n, p->held ? "RELEASED" : "passed at once (the other PC was there first)",
             (unsigned)(now - p->t0), p->held);
        return RB_PROCEED;
    }
    uint32_t limit = kind == RB_SECTION ? b->cfg.section_ms : kind == RB_LSTART ? b->cfg.lstart_ms : b->cfg.pcs_ms;
    if ((uint32_t)(now - p->t0) >= limit) {
        p->st = 3, b->released_timeout++, b->k_timeout[kind]++;
        blog(b, "barrier: %s #%u RELEASED by the TIMEOUT after %u ms, %u frames held (the other PC never arrived)", kind_name(kind), p->n, (unsigned)(now - p->t0), p->held);
        return RB_PROCEED;
    }
    if (!p->held) {
        b->k_episodes[kind]++;
        blog(b, "barrier: %s #%u HOLD begins: this PC waits for the other one (limit %u ms)", kind_name(kind), p->n, limit);
    }
    p->held++;
    b->holds++, b->k_holds[kind]++;
    return RB_HOLD;
}

void rb_section_end_seen(rb_t *b) { b->section_ends++; }

int rb_section_gate(rb_t *b)
{
    rb_sinfo si;
    uint8_t body[RB_MSG_MAX];
    if (!b->enabled || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) return rb_gate(b, RB_SECTION, 0);   /* no session: rb_gate logs it and goes on */
    uint32_t now = b->io.now(b->io.user);
    rb_point *p = &b->pt[RB_SECTION];
    if (p->st != 0 && (uint32_t)(now - p->last) > b->cfg.latch_ms) p->st = 0;   /* not asked for a while: a new visit */
    if (p->st == 0) {
        p->key = 0, p->n = occurrence(b, RB_SECTION, 0), p->t0 = now, p->st = 1, p->held = 0;
        int len = rb_msg_arrive(body, sizeof body, RB_SECTION, 0, p->n);
        if (!len || !b->io.send || !b->io.send(b->io.user, body, (unsigned)len)) {
            p->last = now;
            blog(b, "barrier: section end #%u: the arrival could not be sent, goes on", p->n);
            return RB_PROCEED;
        }
        b->arrivals_tx++;
        blog(b, "barrier: section end #%u reached, arrival sent", p->n);
    }
    p->last = now;
    if (p->st == 1 && peer_has(b, RB_SECTION, 0, p->n)) {
        p->st = 2, b->released_peer++, b->k_peer[RB_SECTION]++;
        blog(b, "barrier: section end #%u: the other PC was there first, both go on", p->n);
    } else if (p->st == 1) {
        b->sec_nohold++;
        blog(b, "barrier: section end #%u: goes on WITHOUT a hold (cEpisodeEndState_vf7 runs once per section end; the next level's load barrier brings the PCs "
                "together, a PC that does not reach its end follows the SECTION)", p->n);
    }
    return RB_PROCEED;
}

int rb_pcs_is_cutscene(const char *r)
{
    if (!r) return 0;
    return (r[0] == 'd' || r[0] == 'D') && (r[1] == 'e' || r[1] == 'E') && (r[2] == 'm' || r[2] == 'M') && (r[3] == 'o' || r[3] == 'O') && (r[4] == '\\' || r[4] == '/');
}

void rb_pcs_begin(rb_t *b, uint32_t key, int real_cutscene)
{
    b->pcs_begins++;
    if (!real_cutscene) {   /* a scene script (the wave sound loop of 1-1 restarts every 8.3 s): no rendezvous, the pending cutscene stays as it is */
        b->pcs_ambient++;
        return;
    }
    if (!b->enabled) return;
    b->pcs_pending = 1, b->pcs_key = key;
    b->pt[RB_PCS].st = 0;   /* a new cutscene meets afresh */
}

void rb_movie_begin(rb_t *b, uint32_t key)
{
    (void)key;
    b->movie_begins++;
}

int rb_pcs_end(rb_t *b, int engine_ended)
{
    if (!engine_ended) return 0;
    if (!b->enabled || !b->pcs_pending) return 1;
    if (rb_gate(b, RB_PCS, b->pcs_key) == RB_HOLD) return 0;
    b->pcs_pending = 0;
    return 1;
}

uint32_t rb_pcs_filter(rb_t *b, uint32_t engine_eax)
{
    int ended = (engine_eax & 0xFFu) != 0;
    int r = rb_pcs_end(b, ended);
    if (r == ended) return engine_eax;   /* bit for bit what the game computed */
    b->pcs_changed++;
    return (engine_eax & ~0xFFu) | (uint32_t)(r ? 1 : 0);
}

/* ---- the level load barrier (the session's LOAD_BEGIN / GO) ---- */
void rb_load_step(rb_t *b, int playable)
{
    rb_sinfo si;
    memset(&si, 0, sizeof si);   /* an io without the other PC's load area leaves it 0 (= not known) */
    if (!b->enabled || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) {
        b->first_load = 1, b->seen_playable = 0, b->epoch_valid = 0, b->reported_valid = 0, b->own_began = 0;
        b->play_since = b->unplay_since = 0;
        return;
    }
    uint32_t now = b->io.now(b->io.user);
    if (playable) {
        b->unplay_since = 0;
        if (!b->play_since) b->play_since = now ? now : 1;
    } else {
        b->play_since = 0;
        if (!b->unplay_since) b->unplay_since = now ? now : 1;
    }
    int gone = !playable && b->unplay_since && (uint32_t)(now - b->unplay_since) >= b->cfg.unplay_ms;
    if (si.phase == PHASE_LOADING) {
        if (!b->epoch_valid || si.epoch != b->epoch_seen) {   /* a new level instance began (by us, or by the other PC) */
            b->epoch_valid = 1, b->epoch_seen = si.epoch, b->epoch_t0 = now;
            b->reported_valid = 0;
            b->epoch_first = b->first_load;
            b->first_load = 0;
            b->unplayed_in_epoch = b->own_began;
            b->own_began = 0;
            b->seen_playable = 0;
        }
        if (gone) b->unplayed_in_epoch = 1;
        int may = b->epoch_first || b->unplayed_in_epoch;
        if (!may && (uint32_t)(now - b->epoch_t0) >= b->cfg.stale_epoch_ms) {
            /* rev1 live 8 (ef4 07:57:18): the client had gone on into 1-3 while the host's level (1-2) stayed; the host reported the new epoch loaded anyway, both
             * PCs PLAYED one epoch in two levels: the 1-3 Jill puppet was teleported to the host's 1-2 spot, the host's 1-2 digest cleared 1-3's start flags, and the
             * client crashed (exe+9d344) when it tore that level down. A level that stays is only "the same level" when the other PC loads the same section. */
            int other_sec = si.other_area && si.other_area_epoch == si.epoch ? (int)((si.other_area >> 8) & 0xFFFFu) : -1;
            int own_sec = b->lvl_area ? (int)((b->lvl_area >> 8) & 0xFFFFu) : -1;
            if (b->cfg.xl_guard && other_sec >= 0 && own_sec >= 0 && other_sec != own_sec) {
                if (b->xl_epoch != si.epoch + 1u) {
                    b->xl_epoch = si.epoch + 1u, b->loads_xl++;
                    blog(b, "barrier: epoch %u: the other PC loads area %08X, this PC's level is %08X: NOT reported loaded (one epoch must not hold two levels); this PC waits for its own change or the follow",
                         si.epoch, si.other_area, b->lvl_area);
                }
            } else {
                may = 1;
                b->unplayed_in_epoch = 1;   /* ef4: decided once per epoch (the line came on every frame while the other PC loaded for minutes) */
                b->loads_stale++;
                blog(b, "barrier: epoch %u started by the other PC while this level stayed for %u ms: reporting loaded anyway", si.epoch, (unsigned)(now - b->epoch_t0));
            }
        }
        if (playable && b->play_since && (uint32_t)(now - b->play_since) >= b->cfg.settle_ms && may && !(b->reported_valid && b->reported_epoch == si.epoch)) {
            b->io.load_done(b->io.user);
            b->reported_valid = 1, b->reported_epoch = si.epoch, b->loads_done++;
            b->cont_t = 0, b->cont_ours = 0;   /* continue1: a level loaded: the next stock Continue request is a new one */
            blog(b, "barrier: LOAD_DONE for epoch %u (level playable)", si.epoch);
        }
    } else if (si.phase == PHASE_PLAYING) {
        if (playable) b->seen_playable = 1;
        else if (b->seen_playable && gone) {
            b->own_began = 1, b->seen_playable = 0, b->loads_begun++;
            b->io.load_begin(b->io.user, b->lvl_area);   /* rev1 live 8: the level this PC goes to (0 = not known) */
            blog(b, "barrier: LOAD_BEGIN (this level went away) at epoch %u", si.epoch);
        }
    }
    b->play_prev = playable;
}

/* ================================================================ 4. restart */
/* continue1 (B): this PC's restart (its own Continue with the host's triple, or the host's own): its next level start meets the other PC's, and its humans are
 * shielded until the session's barrier of the next level instance opens. key = the RESTART's number (the host's rs_tx_seq = the guest's rs_seq). */
#define RB_LS_KEY(seq) (0x52530000u | (uint32_t)(uint16_t)(seq))
static void restart_arm(rb_t *b, uint16_t seq, const char *who)
{
    rb_sinfo si;
    memset(&si, 0, sizeof si);
    if (b->io.session) b->io.session(b->io.user, &si);
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    if (b->cfg.lstart_hold) {
        b->ls_armed = 1, b->ls_key = RB_LS_KEY(seq), b->ls_t = now, b->ls_arms++;
        b->pt[RB_LSTART].st = 0;   /* a new visit of the level start */
    }
    if (b->cfg.shield) b->sh_armed = 1, b->sh_t = now, b->sh_epoch0 = si.epoch, b->sh_loading = si.phase == PHASE_LOADING, b->sh_arms++;
    if (b->cfg.lstart_hold || b->cfg.shield)
        blog(b, "barrier: restart #%u (%s): %s%s%s (session epoch %u, phase %d)", seq, who, b->cfg.lstart_hold ? "the reloaded level starts together with the other PC's" : "",
             b->cfg.lstart_hold && b->cfg.shield ? "; " : "", b->cfg.shield ? "the own humans take no hit until the barrier opens" : "", si.epoch, si.phase);
}

int rb_cp_rec_take(uint32_t w[9], const rb_triple *t, const rb_cp *cp)
{
    if (!w || !t || !cp || !cp->have) return 0;
    uint32_t old[9];
    memcpy(old, w, sizeof old);
    w[4] = cp->mask, w[5] = t->area, w[6] = (uint32_t)cp->id, w[7] = (uint32_t)t->a, w[8] = (uint32_t)t->b;
    w[2] &= ~2u;   /* FUN_006d1a70: a passed checkpoint clears bit 1 of +8 */
    return memcmp(old, w, sizeof old) != 0;
}

int rb_restart_host(rb_t *b, const rb_triple *t)
{
    uint8_t body[RB_MSG_MAX];
    char why[100];
    rb_sinfo si;
    if (!b->enabled || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) return 0;
    if (!rb_triple_ok(t, why, sizeof why)) {
        blog(b, "barrier: host restart (area %08X, %d, %d) NOT sent: %s", t->area, t->a, t->b, why);
        return 0;
    }
    if (b->cfg.ep1_only && !rb_ep1_area(t->area)) {   /* the product supports Episode 1 only: the partner does not follow, the glue ends the session */
        b->st_ep1++;
        blog(b, "barrier: host restart / start in area %08X is outside Episode 1: not sent (the session ends)", t->area);
        return RB_ST_EP1;
    }
    b->lvl_area = t->area;   /* rev1 live 8: this PC loads that section now */
    rb_cp cp = b->tx_cp;
    if (!b->cfg.cp_sync) cp.have = 0;
    /* rev1 live 15: the start kind (rb_start_triple's caller; the restart hook = a save's checkpoint: SAVE) travels with the triple when the feature is on */
    int kind = (b->tx_kind_once & 0x100) ? (b->tx_kind_once & 3) : RB_KIND_SAVE;
    b->tx_kind_once = 0, b->tx_kind = kind;
    int len = b->cfg.title_load ? rb_msg_restart_k(body, sizeof body, ++b->rs_tx_seq, t->area, t->a, t->b, b->tx_diff, &cp, kind)
                                : rb_msg_restart_cp(body, sizeof body, ++b->rs_tx_seq, t->area, t->a, t->b, b->tx_diff, &cp);
    if (!len || !b->io.send || !b->io.send(b->io.user, body, (unsigned)len)) {
        blog(b, "barrier: host restart could not be sent");
        return 0;
    }
    b->rs_sent++;
    b->cp_sent += len == RB_RESTART_CP_LEN || (len == RB_RESTART_K_LEN && cp.have);
    b->tx_have = 1, b->tx_last = *t, b->tx_t = b->io.now ? b->io.now(b->io.user) : 0;
    if (len == RB_RESTART_CP_LEN || (len == RB_RESTART_K_LEN && cp.have))
        blog(b, "barrier: host restart #%u sent: area %08X checkpoints %d / %d, difficulty %d, checkpoint record mID %d mask %04X%s", b->rs_tx_seq, t->area, t->a, t->b,
             b->tx_diff >= 0 && b->tx_diff <= RB_DIFF_MAX ? b->tx_diff : -1, cp.id, cp.mask & 0xFFFFu,
             len != RB_RESTART_K_LEN ? "" : kind == RB_KIND_SAVE ? ", start kind SAVE" : kind == RB_KIND_NEW ? ", start kind NEW GAME" : ", start kind unknown");
    else
        blog(b, "barrier: host restart #%u sent: area %08X checkpoints %d / %d, difficulty %d", b->rs_tx_seq, t->area, t->a, t->b, len == 17 ? b->tx_diff : -1);
    restart_arm(b, b->rs_tx_seq, "the host's own");
    return 1;
}

int rb_restart_client(rb_t *b, rb_triple *inout)
{
    rb_sinfo si;
    b->applied_diff = -1;
    if (!b->enabled || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) {
        snprintf(b->why, sizeof b->why, "restart: no network session, own checkpoint used");
        return 0;
    }
    uint32_t now = b->io.now(b->io.user);
    if (b->rs_have && !b->sec_pending && (uint32_t)(now - b->rs_t) > TRIPLE_FRESH_MS) {   /* (a section triple stays until it is used: the host has left that section for good) */
        b->rs_have = 0;
        blog(b, "barrier: the host's triple is %u ms old, dropped", (unsigned)(now - b->rs_t));
    }
    b->applied_cp.have = 0;
    if (!b->rs_have) {
        b->rs_unmatched++;
        /* continue1 (C): a stock Continue's restart call right after one that took the host's triple (the guest's own Yes in flight while the follow called
         * directly): that reload is on its way; this one is dropped (else the guest would reload again, at its own checkpoint) */
        if (b->cfg.cont_req && b->call_stock && b->rs_last_have && (uint32_t)(now - b->rs_last_t) < RB_RESTART_DUP_MS) {
            b->rs_dups++;
            blog(b, "barrier: a second restart call (own area %08X, %d, %d) %u ms after the one with the host's triple #%u: dropped (that reload is on its way)", inout->area, inout->a,
                 inout->b, (unsigned)(now - b->rs_last_t), b->rs_last_seq);
            return -1;
        }
        /* continue1 (D): this PC's own Continue (its game-over Yes / pause menu) asked the host (RBM_CONTREQ): the restart waits for the host's triple */
        if (b->cfg.cont_req && b->call_stock && !b->held_fallback && b->cont_t && !b->cont_ours && (uint32_t)(now - b->cont_t) < b->cfg.cont_wait_ms) {
            if (!b->held) {
                b->held = 1, b->held_own = *inout, b->held_t = now ? now : 1, b->held_count++;
                blog(b, "barrier: this PC's own Continue (area %08X, %d, %d) waits for the host's triple (CONTREQ sent %u ms ago; at most %u ms, then its own checkpoint)", inout->area,
                     inout->a, inout->b, (unsigned)(now - b->cont_t), (unsigned)b->cfg.cont_wait_ms);
            }
            return -1;
        }
        if (b->held_fallback) {
            b->held_fallback = 0, b->held_own_used++;
            blog(b, "barrier: no host triple came for this PC's own Continue: its own checkpoint is used (area %08X, %d, %d)", inout->area, inout->a, inout->b);
        }
        if (b->cfg.no_triple_hold_ms > 0) {
            if (!b->rs_wait_t0) b->rs_wait_t0 = now ? now : 1;
            if ((uint32_t)(now - b->rs_wait_t0) < (uint32_t)b->cfg.no_triple_hold_ms) {
                snprintf(b->why, sizeof b->why, "restart: waiting for the host's triple (%u ms)", (unsigned)(now - b->rs_wait_t0));
                return -1;
            }
        }
        b->rs_wait_t0 = 0;
        b->lvl_area = inout->area;   /* rev1 live 8 */
        blog(b, "barrier: restart without a host triple: the own checkpoint stays (area %08X, %d, %d)", inout->area, inout->a, inout->b);
        return 0;
    }
    b->rs_wait_t0 = 0;
    blog(b, "barrier: client restart rewritten: own (area %08X, %d, %d) -> host's %s (area %08X, %d, %d)", inout->area, inout->a, inout->b, b->sec_pending ? "next section" : "triple", b->rs_area,
         b->rs_a, b->rs_b);
    inout->area = b->rs_area, inout->a = b->rs_a, inout->b = b->rs_b;
    b->lvl_area = b->rs_area;   /* rev1 live 8 */
    b->applied_diff = b->rs_diff;
    int was_section = b->sec_pending;
    b->rs_have = 0, b->rs_applied++;
    b->sec_pending = 0;
    if (!was_section) {   /* continue1: a Continue / story start of the host (the SECTION follow is not touched here) */
        if (b->cfg.cp_sync && b->rs_cp.have) b->applied_cp = b->rs_cp, b->cp_applied++;
        b->rs_last = *inout, b->rs_last_t = now ? now : 1, b->rs_last_have = 1, b->rs_last_seq = b->rs_seq;
        b->held = 0, b->held_fallback = 0;
        restart_arm(b, b->rs_seq, "the host's triple");
    }
    return 1;
}

int rb_restart_follow(rb_t *b, rb_triple *out) { return rb_restart_follow_at(b, out, 0); }

int rb_restart_follow_at(rb_t *b, rb_triple *out, int at_title)
{
    rb_sinfo si;
    if (!b->enabled || b->host || !b->rs_have || b->cfg.follow_ms <= 0 || !b->io.now || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) return 0;
    uint32_t now = b->io.now(b->io.user);
    uint32_t age = (uint32_t)(now - b->rs_t);
    if (b->ep_local || b->ep_peer) {   /* E5: never into Episode 2 */
        b->rs_have = 0, b->sec_pending = 0;
        return 0;
    }
    if (b->sec_pending) {   /* E4: the host's next section */
        if (b->own_sec_have && b->own_sec_area == b->rs_area && (int32_t)(now - b->own_sec_t) <= b->cfg.sec_window_ms) {
            b->rs_have = 0, b->sec_pending = 0, b->sec_consumed++;
            blog(b, "barrier: the host's next section %08X: this PC changed there by itself (%d ms after the triple), no follow", b->rs_area, (int)(b->own_sec_t - b->rs_t));
            return 0;
        }
        const rb_point *p = &b->pt[RB_SECTION];
        int at_end = p->st != 0 && (int32_t)(now - p->last) <= b->cfg.sec_wait_ms;   /* this PC's own section end ran just now: its own change comes */
        if (at_end && age < (uint32_t)b->cfg.sec_wait_ms) return 0;
        if (age < (uint32_t)b->cfg.sec_follow_ms || (b->sec_follow_valid && b->sec_follow_seq == b->sec_rx_seq)) return 0;
        /* rev1 live 10: FUN_00720870 into another section (from the running level, l8c, or from the title after FUN_008B1660, l9b / l9d) loads the new
         * section WITHOUT its scene scripts (the restart bits + this PC's old checkpoint slot: the reload branch of FUN_0041C8D0). A PC still in the section
         * the host left ends it by its own stock section end instead: the same section change as the host's (the next section of its own level), the
         * checkpoint slot cleared, the result screen, the bag carried. rb_section_change consumes the triple when the change comes. */
        int in_left = !at_title && b->lvl_area && b->sec_from && ((b->lvl_area >> 8) & 0xFFFFu) == ((b->sec_from >> 8) & 0xFFFFu);
        if (b->cfg.sec_stock && in_left) {
            if (!b->sec_stock_asked || b->sec_stock_seq != b->sec_rx_seq) {
                b->sec_stock_asked = 1, b->sec_stock_seq = b->sec_rx_seq, b->sec_stock_t = now ? now : 1, b->sec_stock_count++;
                blog(b, "barrier: this PC did not reach the end of %08X (%s): the host's next section %08X comes by this PC's OWN STOCK section end (the section-end game flags), %u ms after the triple",
                     b->sec_from, at_end ? "its own section end is still pending" : "it did not reach the end", b->rs_area, (unsigned)age);
                return RB_FOL_SECEND;
            }
            if ((uint32_t)(now - b->sec_stock_t) < RB_SECEND_GIVEUP_MS) return 0;   /* the stock end is on its way */
            b->sec_stock_fallback++;
            blog(b, "barrier: the stock section end did not come within %u ms: this PC follows with the restart call after all", (unsigned)RB_SECEND_GIVEUP_MS);
        }
        b->sec_follow_seq = b->sec_rx_seq, b->sec_follow_valid = 1;
        b->rs_followed++, b->sec_followed++;
        out->area = b->rs_area, out->a = b->rs_a, out->b = b->rs_b;
        blog(b, "barrier: this PC did not change the section with the host (%s): it follows the host from %08X into %08X (start %d / %d), %u ms after the triple%s",
             at_end ? "its own section end is still pending" : "it did not reach the end", b->sec_from, b->rs_area, b->rs_a, b->rs_b, (unsigned)age,
             at_title ? " (at the title)" : "");
        return RB_FOL_CALL;
    }
    if (b->held) return 0;   /* continue1 (D): this PC's own Continue waits for this triple: rb_held_step loads it at once */
    /* continue1 (C): this PC's own Continue (its menu) is on its way: its restart takes the triple (a follow now would reload while its checkpoint read runs);
     * after stock_wait_ms the follow goes on as before */
    if (b->cont_t && !b->cont_ours && (uint32_t)(now - b->cont_t) < b->cfg.stock_wait_ms) return 0;
    if (age < (uint32_t)b->cfg.follow_ms || (!at_title && age > TRIPLE_FRESH_MS)) return 0;
    if (b->rs_follow_seq == b->rs_seq) {
        /* continue1 (C): the follow asked the stock Continue for this triple and it has not reached the restart: the direct call after all (once) */
        if (b->fol_stock && b->fol_stock_seq == b->rs_seq && !b->fol_direct_done && (uint32_t)(now - b->fol_stock_t) >= b->cfg.stock_wait_ms) {
            b->fol_direct_done = 1, b->fol_stock_fallback++, b->fol_try_stock = 0;
            out->area = b->rs_area, out->a = b->rs_a, out->b = b->rs_b;
            blog(b, "barrier: the stock Continue asked %u ms ago did not use the host's triple #%u: the direct call now (area %08X, %d, %d)", (unsigned)(now - b->fol_stock_t), b->rs_seq,
                 b->rs_area, b->rs_a, b->rs_b);
            return 1;
        }
        return 0;
    }
    b->rs_follow_seq = b->rs_seq;
    b->rs_followed++;
    /* c1a live: the stock Continue loads this PC's own save (flags, record) during the load: only when that save holds the host's checkpoint */
    rb_cprec hc = {b->rs_cp.have, b->rs_cp.id, b->rs_cp.mask, b->rs_area, b->rs_a, b->rs_b};
    int saved_ok = cp_same(&b->cp_saved, &hc);
    /* rev1 live 14 (m14h / m14i, LIVE): at the title this PC's saved checkpoint is not known: rb_cpm_step took the title's CLEARED record (area 91ff0000,
     * -1 / -1, mID -1) as cp_saved, so saved_ok was always 0 there and titlestock never acted (the guest's Jill again without a weapon; r45 covered only
     * cp_saved.have 0). With titlestock an unknown or cleared record counts: its own stock Continue reads the save (its bag), the restart hook still takes
     * the host's triple, and after stock_wait_ms the direct call comes after all; a known level record of another checkpoint stays direct */
    if (at_title && b->cfg.title_stock && (!b->cp_saved.have || b->cp_saved.area == RB_AREA_NONE)) saved_ok = 1;
    b->fol_try_stock = b->cfg.follow_stock && (!at_title || b->cfg.title_stock) && saved_ok, b->fol_stock = 0, b->fol_direct_done = 0;   /* rev1 live 14: title_stock */
    if (b->cfg.follow_stock && !at_title && !saved_ok) b->stock_refused++;
    out->area = b->rs_area, out->a = b->rs_a, out->b = b->rs_b;
    blog(b, "barrier: no own Continue used the host's triple within %u ms: the client restarts by itself (area %08X, %d, %d)%s%s", (unsigned)age, b->rs_area, b->rs_a, b->rs_b,
         b->fol_try_stock ? ", through its own stock Continue when it can (its save holds the host's checkpoint)" : "",
         b->cfg.follow_stock && !at_title && !saved_ok ? ", by the direct call: this PC's save does not hold the host's checkpoint (the stock Continue would load its older save)" : "");
    if (b->cfg.follow_stock && !at_title && !saved_ok)
        blog(b, "barrier: this PC's saved checkpoint: mID %d area %08X %d / %d (have %d); the host's: mID %d area %08X %d / %d (have %d)", b->cp_saved.id, b->cp_saved.area, b->cp_saved.a,
             b->cp_saved.b, b->cp_saved.have, hc.id, hc.area, hc.a, hc.b, hc.have);
    return 1;
}

/* ---- continue1 ---- */
void rb_follow_went(rb_t *b, int stock)
{
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    b->fol_try_stock = 0;
    if (!stock) return;
    b->fol_stock = 1, b->fol_stock_seq = b->rs_seq, b->fol_stock_t = now, b->fol_stock_asked++;
    blog(b, "barrier: the follow asks this PC's own stock Continue (FUN_0041efe0(0): aStartContinue, its checkpoint read, then FUN_00720870 through the restart hook with the host's triple #%u)",
         b->rs_seq);
}

/* ---- rev1 live 15: the guest's own save read before its follow at the title ---- */
int rb_start_kind(uint32_t ret_addr)
{
    if (ret_addr >= RB_SCP_FN_LO && ret_addr < RB_SCP_FN_HI) return RB_KIND_SAVE;
    if (ret_addr >= RB_TITLE_FN_LO && ret_addr < RB_TITLE_FN_HI) return RB_KIND_NEW;
    return RB_KIND_UNKNOWN;
}

int rb_title_load_gate(rb_t *b, const rb_triple *fol, int at_title)
{
    if (!at_title || b->host || !b->cfg.title_load) return RB_TL_CALL;
    if (b->tl_state == RB_TLS_ASK || b->tl_state == RB_TLS_WAIT) return RB_TL_WAIT;   /* a retry while the read runs */
    if (b->tl_state == RB_TLS_DONE) return RB_TL_CALL;                                /* this title stay has read its save already */
    if (b->sec_pending || b->rs_kind != RB_KIND_SAVE) {
        b->tl_skipped++;
        blog(b, "barrier: title follow: the host's start is %s: no own save read (the call now)", b->sec_pending ? "a section change" : b->rs_kind == RB_KIND_NEW ? "a NEW GAME" : "of an unknown kind");
        b->tl_state = RB_TLS_DONE;
        return RB_TL_CALL;
    }
    b->tl_state = RB_TLS_ASK, b->tl_fol = *fol, b->tl_t = b->io.now ? b->io.now(b->io.user) : 0, b->tl_asked++;
    blog(b, "barrier: title follow: the host's start is a SAVE (its title Continue): this PC reads its own save first, the stock way (its bag, flags, record), then follows into %08X %d / %d",
         fol->area, fol->a, fol->b);
    return RB_TL_LOAD;
}

void rb_title_load_asked(rb_t *b, int asked, const char *why)
{
    if (b->tl_state != RB_TLS_ASK) return;
    if (asked) {
        b->tl_state = RB_TLS_WAIT;
        return;
    }
    b->tl_state = RB_TLS_DONE, b->tl_refused++;
    blog(b, "barrier: title follow: this PC's save read could not be asked (%s): the direct call now", why ? why : "?");
}

int rb_title_load_frame(rb_t *b, int at_title, int done, int ok, rb_triple *out)
{
    if (!at_title) {
        if (b->tl_state == RB_TLS_ASK || b->tl_state == RB_TLS_WAIT) blog(b, "barrier: title follow: a level came while this PC's save read ran (no call from the read)");
        b->tl_state = RB_TLS_IDLE;
        return 0;
    }
    if (b->tl_state != RB_TLS_WAIT) return 0;
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0, age = (uint32_t)(now - b->tl_t);
    if (done) {
        b->tl_state = RB_TLS_DONE;
        if (ok) b->tl_done++;
        else b->tl_failed++;
        blog(b, "barrier: title follow: this PC's own save read %s after %u ms: the follow into %08X %d / %d now", ok ? "is DONE" : "FAILED", (unsigned)age, b->tl_fol.area, b->tl_fol.a,
             b->tl_fol.b);
        *out = b->tl_fol;
        return 1;
    }
    if (age >= b->cfg.title_load_ms) {
        b->tl_state = RB_TLS_DONE, b->tl_timeout++;
        blog(b, "barrier: title follow: this PC's own save read did not finish within %u ms: the direct call now", (unsigned)b->cfg.title_load_ms);
        *out = b->tl_fol;
        return 1;
    }
    return 0;
}

int rb_lstart_gate(rb_t *b, int engine_ready)
{
    if (!engine_ready || !b->ls_armed) return RB_PROCEED;
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    if (!b->cfg.lstart_hold) {
        b->ls_armed = 0;
        return RB_PROCEED;
    }
    if ((uint32_t)(now - b->ls_t) > RB_LS_STALE_MS) {
        b->ls_armed = 0, b->ls_stale++;
        blog(b, "barrier: level start: the restart that armed it is %u ms old: not held", (unsigned)(now - b->ls_t));
        return RB_PROCEED;
    }
    int r = rb_gate(b, RB_LSTART, b->ls_key);
    if (r == RB_PROCEED) {
        b->ls_armed = 0;
        if (b->pt[RB_LSTART].st == 2) b->ls_peer++;
        else if (b->pt[RB_LSTART].st == 3) b->ls_timeout++;
    } else
        b->ls_holds++;
    return r;
}

int rb_shield_on(rb_t *b)
{
    if (!b->sh_armed) return 0;
    rb_sinfo si;
    memset(&si, 0, sizeof si);
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    if (!b->enabled || !b->cfg.shield || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) {
        b->sh_armed = 0;
        return 0;
    }
    if (si.phase == PHASE_LOADING) b->sh_loading = 1;
    if (si.phase == PHASE_PLAYING && (b->sh_loading || si.epoch != b->sh_epoch0)) {
        b->sh_armed = 0, b->sh_opened++;
        blog(b, "barrier: the barrier of epoch %u is open %u ms after the restart: the own humans can be hit again", si.epoch, (unsigned)(now - b->sh_t));
        return 0;
    }
    if ((uint32_t)(now - b->sh_t) >= b->cfg.shield_ms) {
        b->sh_armed = 0, b->sh_capped++;
        blog(b, "barrier: the shield after the restart ends by its limit (%u ms; phase %d epoch %u)", (unsigned)b->cfg.shield_ms, si.phase, si.epoch);
        return 0;
    }
    return 1;
}

int rb_cont_request(rb_t *b, int ours)
{
    rb_sinfo si;
    uint8_t body[RB_MSG_MAX];
    b->cont_calls++;
    if (!b->enabled || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) return RB_PROCEED;   /* solo: the stock game, never dropped */
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    if (b->cont_t && (uint32_t)(now - b->cont_t) < b->cfg.cont_dedup_ms) {
        b->cont_dropped++;
        blog(b, "barrier: a second Continue request (%s) %u ms after the last one (%s), no level loaded since: dropped (a second aStartContinue would assert)", ours ? "the mod's" : "this PC's menu",
             (unsigned)(now - b->cont_t), b->cont_ours ? "the mod's" : "this PC's menu");
        return RB_HOLD;
    }
    b->cont_t = now ? now : 1, b->cont_ours = ours;
    /* c1a live: the guest's own stock Continue would load its own save; when that save does not hold the host's checkpoint, the request is dropped and the
     * guest waits for the host's triple (its held Continue loads it by the direct call; after cont_wait_ms its own checkpoint, also by the direct call) */
    if (!b->host && !ours && b->cfg.cont_req && b->host_cp.have && !cp_same(&b->cp_saved, &b->host_cp)) {
        int sent = 0;
        if (!b->rs_have) {
            int len = rb_msg_contreq(body, sizeof body, ++b->cr_tx_seq);
            sent = len && b->io.send && b->io.send(b->io.user, body, (unsigned)len);
            b->cr_sent += sent != 0;
        }
        b->own_cont_redirected++, b->cont_ours = 1;
        b->held = 1, b->held_t = now ? now : 1, b->held_count++, b->held_fallback = 0;
        b->held_own = b->cp_saved.have ? (rb_triple){b->cp_saved.area, b->cp_saved.a, b->cp_saved.b} : (rb_triple){b->host_cp.area, b->host_cp.a, b->host_cp.b};
        blog(b, "barrier: this PC (guest) chose Continue, but its save holds mID %d (area %08X %d / %d), the host's checkpoint is mID %d (%d / %d): its stock Continue is NOT run; %s; "
             "it loads the host's checkpoint by the direct call when the triple comes (at most %u ms, then its own)", b->cp_saved.id, b->cp_saved.area, b->cp_saved.a, b->cp_saved.b,
             b->host_cp.id, b->host_cp.a, b->host_cp.b, b->rs_have ? "the host's triple is already here" : sent ? "CONTREQ sent" : "CONTREQ could NOT be sent", (unsigned)b->cfg.cont_wait_ms);
        return RB_HOLD;
    }
    if (!b->host && !ours && b->cfg.cont_req && !b->rs_have) {
        int len = rb_msg_contreq(body, sizeof body, ++b->cr_tx_seq);
        int ok = len && b->io.send && b->io.send(b->io.user, body, (unsigned)len);
        b->cr_sent += ok != 0;
        blog(b, "barrier: this PC (guest) chose Continue with no host triple waiting: CONTREQ #%u %s; its restart waits for the host's triple (at most %u ms)", b->cr_tx_seq,
             ok ? "sent" : "could NOT be sent", (unsigned)b->cfg.cont_wait_ms);
        if (!ok) b->cont_ours = 1;   /* nobody will answer: the restart goes on with its own checkpoint at once */
    } else
        blog(b, "barrier: Continue request on this PC (%s, %s)%s", b->host ? "host" : "guest", ours ? "the mod's" : "its own menu",
             !b->host && b->rs_have ? ": the host's triple is waiting, the restart takes it" : "");
    return RB_PROCEED;
}

int rb_cont_request_step(rb_t *b)
{
    if (!b->host || !b->cr_pending) return 0;
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    if ((uint32_t)(now - b->cr_t) > RB_CR_ANSWER_MS) {
        b->cr_pending = 0, b->cr_refused++;
        blog(b, "barrier: the guest's Continue request could not be answered within %u ms (the guest uses its own checkpoint after its wait)", (unsigned)RB_CR_ANSWER_MS);
        return 0;
    }
    if (b->cont_t && (uint32_t)(now - b->cont_t) < b->cfg.cont_dedup_ms) {
        b->cr_pending = 0, b->cr_done++;
        blog(b, "barrier: the guest's Continue request: this PC's own Continue is already on its way (%u ms ago)", (unsigned)(now - b->cont_t));
        return 0;
    }
    if (b->tx_have && (uint32_t)(now - b->tx_t) < RB_CR_ANSWER_MS) {
        b->cr_pending = 0, b->cr_done++;
        blog(b, "barrier: the guest's Continue request: this PC restarted %u ms ago (RESTART #%u went out)", (unsigned)(now - b->tx_t), b->rs_tx_seq);
        return 0;
    }
    return 1;
}

void rb_cont_answered(rb_t *b, int done, const char *why)
{
    if (!done) return;   /* the engine state does not allow it yet: rb_cont_request_step asks again next frame (the glue logs why once) */
    b->cr_pending = 0, b->cr_done++;
    blog(b, "barrier: the guest's Continue request answered: %s", why ? why : "this PC's stock Continue");
}

int rb_held_step(rb_t *b, rb_triple *out, int *host)
{
    if (!b->held) return 0;
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    if (b->rs_have && !b->sec_pending) {
        *out = (rb_triple){b->rs_area, b->rs_a, b->rs_b};
        if (host) *host = 1;
        b->held = 0, b->held_host++;   /* the glue's direct call takes the triple through rb_restart_client (if it cannot call, the normal follow takes it after follow_ms) */
        blog(b, "barrier: the host's triple #%u came %u ms after this PC's own Continue: this PC loads it now (area %08X, %d, %d)", b->rs_seq, (unsigned)(now - b->held_t), b->rs_area,
             b->rs_a, b->rs_b);
        return 1;
    }
    if ((uint32_t)(now - b->held_t) >= b->cfg.cont_wait_ms) {
        *out = b->held_own;
        if (host) *host = 0;
        b->held = 0, b->held_fallback = 1;
        blog(b, "barrier: no host triple within %u ms after this PC's own Continue: it loads its own checkpoint (area %08X, %d, %d)", (unsigned)(now - b->held_t), b->held_own.area,
             b->held_own.a, b->held_own.b);
        return 1;
    }
    return 0;
}

int rb_placed(float x, float y, float z)
{
    if (x != x || y != y || z != z) return 0;   /* NaN */
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
    if (ax > 1e7f || ay > 1e7f || az > 1e7f) return 0;   /* inf / garbage */
    return ax + ay + az > 1.0f;
}

int rb_ep1_area(uint32_t area) { return (area & 0xFFFF0000u) == 0x91010000u && (area & 0xFF00u) != 0 && (area & 0xFF00u) != 0xFF00u; }

int rb_table_triple(rb_rd32_fn rd, void *user, uint32_t gamesys_ptr, uint32_t area, rb_triple *out)
{
    if (!rd || !out) return 0;
    uint32_t g = rd(user, gamesys_ptr), o = g ? rd(user, g + 0x24u) : 0;
    if (!o) return 0;
    uint32_t tab = rd(user, o + 0x20u) == 0 ? rd(user, o + 0xCu) : rd(user, o + 0x10u);   /* 0x720573: the same pick as FUN_00720540 */
    if (!tab) return 0;
    uint32_t n = rd(user, tab + 0x6Cu), arr = rd(user, tab + 0x78u);
    if (!arr || n > 4096u) return 0;
    for (uint32_t i = 0; i < n; i++) {   /* FUN_00726B70 */
        uint32_t e = rd(user, arr + 4u * i);
        if (!e || rd(user, e + 4u) != area) continue;
        /* entry+8 is the start id 0x92EESS00 (lm2b live: 0x92010100 256 / 1 for 0x91010100); the restart takes the 0x91 area with the entry's checkpoints, as the
         * title Continue does (0x6D11C1..0x6D1212: FUN_00720540 into a local record, then FUN_00720870(the save's area, local +8, local +0xC)) */
        out->area = area, out->a = (int32_t)rd(user, e + 0xCu), out->b = (int32_t)rd(user, e + 0x10u);
        return 1;
    }
    return 0;
}

int rb_bag_shared(rb_rd32_fn rd, rb_isobj_fn is_obj, void *user, uint32_t bag_a, uint32_t bag_b, uint32_t *out, int max)
{
    if (!rd || !bag_a || !bag_b || bag_a == bag_b) return 0;
    int n = 0;
    for (uint32_t i = RB_BAG_FIRST; i < RB_BAG_END; i += 4) {
        uint32_t v = rd(user, bag_a + i);
        if (v <= 0x10000u || (is_obj && !is_obj(user, v))) continue;
        int dup = 0;
        for (uint32_t k = RB_BAG_FIRST; k < i && !dup; k += 4) dup = rd(user, bag_a + k) == v;   /* count each object once */
        if (dup) continue;
        for (uint32_t j = RB_BAG_FIRST; j < RB_BAG_END; j += 4)
            if (rd(user, bag_b + j) == v) {
                if (out && n < max) out[n] = v;
                n++;
                break;
            }
    }
    return n;
}

int rb_start_seen(rb_t *b, uint32_t ret_addr)
{
    b->st_calls++;
    if (!b->enabled || !b->host) return 0;
    if (ret_addr >= RB_SECTION_FN_LO && ret_addr < RB_SECTION_FN_HI) {   /* the section change: both PCs advance through the epend rendezvous themselves */
        b->st_section++;
        return 0;
    }
    rb_sinfo si;
    if (!b->io.session || !b->io.session(b->io.user, &si) || !si.started) return 0;
    b->st_pending = 1, b->st_ret = ret_addr, b->st_t = b->io.now ? b->io.now(b->io.user) : 0;
    return 1;
}

int rb_start_triple(rb_t *b, const rb_triple *rec)
{
    if (!b->st_pending) return RB_ST_NONE;
    b->st_pending = 0;
    char why[100];
    if (!rec || !rb_triple_ok(rec, why, sizeof why)) {
        b->st_bad++;
        if (rec) blog(b, "barrier: host story start (from %08X): the start triple area %08X %d / %d is not plausible (%s): not sent", b->st_ret, rec->area, rec->a, rec->b, why);
        else blog(b, "barrier: host story start (from %08X): no start triple: not sent", b->st_ret);
        return RB_ST_BAD;
    }
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    if (b->tx_have && b->tx_last.area == rec->area && b->tx_last.a == rec->a && b->tx_last.b == rec->b && (uint32_t)(now - b->tx_t) < 3000u) {
        b->st_dup++;   /* the restart hook of the same start (title Continue of 1-1 / 1-3, the checkpoint reload) sent it already */
        return RB_ST_DUP;
    }
    b->tx_kind_once = 0x100 | rb_start_kind(b->st_ret);   /* rev1 live 15 */
    int r = rb_restart_host(b, rec);
    b->tx_kind_once = 0;
    if (r == 1) {
        b->st_sent++;
        blog(b, "barrier: host story start (FUN_00720540 from %08X): area %08X checkpoints %d / %d sent: the client loads it by itself", b->st_ret, rec->area, rec->a, rec->b);
        return RB_ST_SENT;
    }
    return r == RB_ST_EP1 ? RB_ST_EP1 : RB_ST_NONE;
}

void rb_restart_inject(rb_t *b, const rb_triple *t)
{
    b->rs_have = 1, b->rs_area = t->area, b->rs_a = t->a, b->rs_b = t->b, b->rs_seq++, b->rs_t = b->io.now ? b->io.now(b->io.user) : 0;
    b->rs_received++;
    b->sec_pending = 0;
}

/* ================================================================ 5. ep1-flow: the section change (E4) and the end of Episode 1 (E5) */
int rb_msg_section(uint8_t *out, size_t cap, uint16_t seq, const rb_triple *next, uint32_t from, int how)
{
    if (cap < RB_SECTION_MSG_LEN || !next) return 0;
    out[0] = RB_MSG_VER, out[1] = RBM_SECTION;
    memcpy(out + 2, &seq, 2);
    memcpy(out + 4, &next->area, 4);
    memcpy(out + 8, &next->a, 4);
    memcpy(out + 12, &next->b, 4);
    memcpy(out + 16, &from, 4);
    out[20] = (uint8_t)how;
    return RB_SECTION_MSG_LEN;
}

int rb_msg_epdone(uint8_t *out, size_t cap, uint32_t from)
{
    if (cap < RB_EPDONE_MSG_LEN) return 0;
    out[0] = RB_MSG_VER, out[1] = RBM_EPDONE;
    memcpy(out + 2, &from, 4);
    return RB_EPDONE_MSG_LEN;
}

static const char *how_text(int how) { return how == RB_SEC_PEER ? "released by the other PC" : how == RB_SEC_TIMEOUT ? "released by its TIMEOUT" : "no rendezvous ran"; }

int rb_section_change(rb_t *b, uint32_t from, uint32_t next, const rb_triple *next_start)
{
    rb_sinfo si;
    uint8_t body[RB_MSG_MAX];
    char why[100];
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    b->sec_changes++;
    if (!b->enabled || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) return RB_SC_NONE;
    int from_ep1 = from ? rb_ep1_area(from) : b->cfg.ep1_only;   /* an unreadable section: the product plays Episode 1 only */
    if (!from_ep1) return RB_SC_NONE;   /* a lab session outside Episode 1 (REV1_ANYEPISODE=1): each PC goes on by itself, as before ep1-flow */
    b->own_sec_have = 1, b->own_sec_area = next, b->own_sec_t = now, b->own_sec_from = from;
    b->lvl_area = next;   /* rev1 live 8 */
    if (!rb_ep1_area(next)) {   /* E5: the end of Episode 1 (1-3 -> the next episode) */
        if (!b->ep_local) b->ep_local = 1, b->ep_local_t = now ? now : 1, b->ep_from = from, b->ep_seen_flow = 0, b->ep_closed = 0;
        if (!b->host && (b->rs_have || b->sec_pending)) blog(b, "barrier: a waiting host triple (area %08X) is dropped: Episode 1 has ended", b->rs_area);
        if (!b->host) b->rs_have = 0, b->sec_pending = 0;
        int len = rb_msg_epdone(body, sizeof body, from);
        int ok = len && b->io.send && b->io.send(b->io.user, body, (unsigned)len);
        b->ep_sent += ok != 0;
        blog(b, "barrier: END OF EPISODE 1 on this PC (%s, section end %s): %08X -> %08X; %s; the session closes after the stock end flow (result, save)", b->host ? "host" : "client",
             how_text(b->pt[RB_SECTION].st), from, next, ok ? "the partner is told (EPDONE)" : "EPDONE could NOT be sent");
        return RB_SC_EPEND;
    }
    if (!b->host) {
        if (b->sec_pending && b->rs_area == next) {
            b->rs_have = 0, b->sec_pending = 0, b->sec_consumed++;
            blog(b, "barrier: section change on this PC (client, %s): %08X -> %08X = the host's waiting next section: no follow needed", how_text(b->pt[RB_SECTION].st), from, next);
        } else
            blog(b, "barrier: section change on this PC (client, %s): %08X -> %08X", how_text(b->pt[RB_SECTION].st), from, next);
        return RB_SC_NOTED;
    }
    if (!next_start || next_start->area != next || !rb_triple_ok(next_start, why, sizeof why)) {
        b->sec_bad++;
        blog(b, "barrier: host section change %08X -> %08X: NO start triple for the client (%s): not sent", from, next, next_start ? why : "the area is not in the start table");
        return RB_SC_BAD;
    }
    int how = b->pt[RB_SECTION].st == 2 ? RB_SEC_PEER : b->pt[RB_SECTION].st == 3 ? RB_SEC_TIMEOUT : RB_SEC_ALONE;
    int len = rb_msg_section(body, sizeof body, ++b->sec_tx_seq, next_start, from, how);
    if (!len || !b->io.send || !b->io.send(b->io.user, body, (unsigned)len)) {
        b->sec_bad++;
        blog(b, "barrier: host section change %08X -> %08X: the next section's triple could not be sent", from, next);
        return RB_SC_BAD;
    }
    b->sec_sent++;
    blog(b, "barrier: host section change %08X -> %08X (section end %s): SECTION #%u sent, the client's start %d / %d", from, next, how_text(how), b->sec_tx_seq, next_start->a, next_start->b);
    return RB_SC_SENT;
}

int rb_ep_close_step(rb_t *b, int in_end_flow, int level_out, uint32_t now)
{
    if (!b->ep_local || b->ep_closed) return 0;
    if (in_end_flow > 0 && !b->ep_seen_flow) {
        b->ep_seen_flow = 1;
        blog(b, "barrier: END OF EPISODE 1: the stock end flow runs on this PC (aEpisodeEnd: result, save)");
    }
    const char *why = NULL;
    if (b->ep_seen_flow && in_end_flow == 0) why = "the stock end flow (result, save) is over";
    else if (level_out && (int32_t)(now - b->ep_local_t) > 2000) why = "a level outside Episode 1 is there (the latest point)";
    else if ((uint32_t)(now - b->ep_local_t) >= b->cfg.ep_close_cap_ms) why = "the time limit after the end of Episode 1 passed";
    if (!why) return 0;
    b->ep_closed = 1;
    blog(b, "barrier: END OF EPISODE 1: the session closes now (%s, %u ms after this PC's end)", why, (unsigned)(now - b->ep_local_t));
    return 1;
}

int rb_ep_known(const rb_t *b, int *local)
{
    if (local) *local = b->ep_local;
    return b->ep_local || b->ep_peer;
}

/* ================================================================ 7. continue1 task 4: the checkpoint pass mirror */
int rb_msg_cppass(uint8_t *out, size_t cap, uint16_t seq, const rb_cprec *r)
{
    if (cap < RB_CPPASS_MSG_LEN || !r || !r->have) return 0;
    int16_t id = (int16_t)r->id, a = (int16_t)r->a, bb = (int16_t)r->b;
    uint16_t mask = (uint16_t)(r->mask & 0xFFFFu);
    out[0] = RB_MSG_VER, out[1] = RBM_CPPASS;
    memcpy(out + 2, &id, 2), memcpy(out + 4, &mask, 2), memcpy(out + 6, &r->area, 4), memcpy(out + 10, &a, 2), memcpy(out + 12, &bb, 2), memcpy(out + 14, &seq, 2);
    return RB_CPPASS_MSG_LEN;
}

static int cprec_eq(const rb_cprec *x, const rb_cprec *y)
{
    return x->have == y->have && x->id == y->id && (x->mask & 0xFFFFu) == (y->mask & 0xFFFFu) && x->area == y->area && x->a == y->a && x->b == y->b;
}
#define RB_CPM_KEEP_MS 15000u    /* a partner's pass this PC could not take within this time is dropped */
#define RB_CPM_RETRY_MS 500u     /* the engine refused (its save manager was busy): again after this long */
#define RB_CPM_DELAY_MS 1200u    /* a partner's pass is taken this long after it came: the flags set with it (4390 arms CP mID 1) reach this PC first, so the
                                    checkpoint save written by the pass holds them (CPPASS and the flag stream are separate events) */

int rb_cpm_step(rb_t *b, const rb_cprec *own, int in_level, rb_cprec *out)
{
    rb_sinfo si;
    uint8_t body[RB_MSG_MAX];
    if (own && own->have && !cprec_eq(own, &b->cpm_last) && !(b->cpm_mark.have && cprec_eq(own, &b->cpm_mark))) b->cp_saved = *own;   /* the engine wrote it */
    if (!b->cfg.cp_mirror || !b->enabled || !own || !b->io.session || !b->io.session(b->io.user, &si) || !si.started) {
        if (own) b->cpm_last = *own;
        return 0;
    }
    uint32_t now = b->io.now ? b->io.now(b->io.user) : 0;
    /* this PC's own record moved to a new checkpoint: tell the other PC (not the mod's own writes, not the section start's mID 0, only in a level) */
    if (own->have && !cprec_eq(own, &b->cpm_last)) {
        int marked = b->cpm_mark.have && cprec_eq(own, &b->cpm_mark);
        int from_partner = b->cpm_in.have && own->id == b->cpm_in.id && own->a == b->cpm_in.a && own->b == b->cpm_in.b;
        if (own->id > 0 && own->area != RB_AREA_NONE && in_level && !marked && !from_partner) {
            int len = rb_msg_cppass(body, sizeof body, ++b->cpm_tx_seq, own);
            int ok = len && b->io.send && b->io.send(b->io.user, body, (unsigned)len);
            b->cpm_sent += ok != 0;
            blog(b, "barrier: checkpoint mID %d passed on this PC (area %08X, %d / %d, mask %04X; was mID %d): CPPASS #%u %s", own->id, own->area, own->a, own->b, own->mask & 0xFFFFu,
                 b->cpm_last.id, b->cpm_tx_seq, ok ? "sent" : "could NOT be sent");
        }
        b->cpm_last = *own;
    }
    /* the partner's pass */
    if (!b->cpm_in.have) return 0;
    if (own->have && own->id == b->cpm_in.id && own->a == b->cpm_in.a && own->b == b->cpm_in.b && own->area == b->cpm_in.area) {
        b->cpm_in.have = 0, b->cpm_had++;
        blog(b, "barrier: the partner's checkpoint mID %d: this PC has it (record mID %d, %d / %d)", own->id, own->id, own->a, own->b);
        return 0;
    }
    if (own->have && own->area == b->cpm_in.area && b->cpm_in.id >= 0 && b->cpm_in.id < 32 && (own->mask & (1u << b->cpm_in.id))) {
        b->cpm_in.have = 0, b->cpm_had++;   /* passed here before (the engine refuses it again: FUN_006d1a70's mask test) */
        blog(b, "barrier: the partner's checkpoint mID %d: passed here before (mask %04X, record mID %d): nothing to do", b->cpm_in.id, own->mask & 0xFFFFu, own->id);
        return 0;
    }
    if ((uint32_t)(now - b->cpm_in_t) > RB_CPM_KEEP_MS) {
        b->cpm_in.have = 0, b->cpm_dropped++;
        blog(b, "barrier: the partner's checkpoint mID %d (area %08X) not taken within %u ms (record here mID %d area %08X, in a level %d): dropped", b->cpm_in.id, b->cpm_in.area,
             (unsigned)RB_CPM_KEEP_MS, own->id, own->area, in_level);
        return 0;
    }
    if (!in_level || !own->have) return 0;
    if (own->area != b->cpm_in.area && own->area != RB_AREA_NONE) {   /* another section here: the section follow's business */
        b->cpm_in.have = 0, b->cpm_other++;
        blog(b, "barrier: the partner's checkpoint mID %d is in area %08X, this PC's record in %08X: not taken", b->cpm_in.id, b->cpm_in.area, own->area);
        return 0;
    }
    if ((uint32_t)(now - b->cpm_in_t) < RB_CPM_DELAY_MS) return 0;
    if (b->cpm_try_t && (uint32_t)(now - b->cpm_try_t) < RB_CPM_RETRY_MS) return 0;
    b->cpm_try_t = now ? now : 1;
    *out = b->cpm_in;
    return 1;
}

void rb_cpm_applied(rb_t *b, int called, const rb_cprec *after)
{
    b->cpm_calls += called != 0;
    if (!called || !after) return;
    if (after->have && after->id == b->cpm_in.id && after->a == b->cpm_in.a && after->b == b->cpm_in.b) {
        b->cpm_done++;
        b->cpm_mark = *after;   /* not told back to the partner ... */
        b->cp_saved = *after;   /* ... but the engine wrote it, and its checkpoint save (c1c live: the mark alone left cp_saved at mID 0: the follow went direct) */
        blog(b, "barrier: the partner's checkpoint mID %d passed here too (FUN_006d1a70: record mID %d, mask %04X, area %08X, %d / %d; its checkpoint save follows)", b->cpm_in.id, after->id,
             after->mask & 0xFFFFu, after->area, after->a, after->b);
        b->cpm_in.have = 0;
    }
}

void rb_cpm_mark(rb_t *b, const rb_cprec *r)
{
    if (r) b->cpm_mark = *r;
}

/* ---- status ---- */
void rb_status(const rb_t *b, char *out, size_t size)
{
    static const char *const stn[] = {"idle", "waiting", "released by peer", "released by timeout"};
    snprintf(out, size,
             "rendezvous %s; section %s (n=%u): held frames %u in %u holds, released by peer %u / timeout %u, passes without session %u; "
             "cutscene %s (n=%u, pending %d): held frames %u in %u holds, released by peer %u / timeout %u, passes without session %u, getter answers replaced %u; "
             "arrivals tx %u rx %u; cutscenes begun %u (scene scripts, never held: %u), movies %u, section ends %u; "
             "loads: begun %u, done %u, stale %u, cross-level refused %u (level %08X); restart: host sent %u, client got %u rejected %u used %u without triple %u, followed %u%s; "
             "story starts (FUN_00720540): calls %u, section changes %u, sent %u, duplicates %u, outside Episode 1 %u, bad %u; "
             "ep1-flow: own section changes %u (last %08X -> %08X), SECTION sent %u got %u consumed %u followed %u (stock ends asked %u, fallbacks %u) bad %u%s; end of Episode 1: here %d, partner %d, EPDONE sent %u got %u, "
             "end flow seen %d, closed %d; last: %s",
             b->enabled ? "ON" : "off", stn[b->pt[RB_SECTION].st & 3], b->pt[RB_SECTION].n, b->k_holds[RB_SECTION], b->k_episodes[RB_SECTION], b->k_peer[RB_SECTION], b->k_timeout[RB_SECTION],
             b->k_nosession[RB_SECTION], stn[b->pt[RB_PCS].st & 3], b->pt[RB_PCS].n, b->pcs_pending, b->k_holds[RB_PCS], b->k_episodes[RB_PCS], b->k_peer[RB_PCS], b->k_timeout[RB_PCS],
             b->k_nosession[RB_PCS], b->pcs_changed, b->arrivals_tx, b->arrivals_rx, b->pcs_begins, b->pcs_ambient, b->movie_begins, b->section_ends, b->loads_begun, b->loads_done,
             b->loads_stale, b->loads_xl, b->lvl_area, b->rs_sent, b->rs_received, b->rs_rejected, b->rs_applied, b->rs_unmatched, b->rs_followed, b->rs_have ? " (a triple is waiting)" : "", b->st_calls, b->st_section,
             b->st_sent, b->st_dup, b->st_ep1, b->st_bad, b->sec_changes, b->own_sec_from, b->own_sec_area, b->sec_sent, b->sec_got, b->sec_consumed, b->sec_followed, b->sec_stock_count, b->sec_stock_fallback, b->sec_bad,
             b->sec_pending ? " (a next section is waiting)" : "", b->ep_local, b->ep_peer, b->ep_sent, b->ep_got, b->ep_seen_flow, b->ep_closed, b->why[0] ? b->why : "-");
    size_t n = strlen(out);
    if (n + 1 < size)
        snprintf(out + n, size - n,
                 "; continue1: checkpoint records sent %u got %u applied %u; level start %s (arms %u, held frames %u, by peer %u, timeout %u, stale %u); shield %s (arms %u, opened %u, "
                 "capped %u); follow via the stock Continue asked %u, direct fallbacks %u; second restarts dropped %u; Continue requests %u (dropped %u), CONTREQ sent %u got %u "
                 "answered %u refused %u%s; own Continue held %u (host triple %u, own checkpoint %u)%s",
                 b->cp_sent, b->cp_got, b->cp_applied, b->ls_armed ? "ARMED" : "idle", b->ls_arms, b->ls_holds, b->ls_peer, b->ls_timeout, b->ls_stale, b->sh_armed ? "ON" : "off",
                 b->sh_arms, b->sh_opened, b->sh_capped, b->fol_stock_asked, b->fol_stock_fallback, b->rs_dups, b->cont_calls, b->cont_dropped, b->cr_sent, b->cr_got, b->cr_done,
                 b->cr_refused, b->cr_pending ? " (one waiting)" : "", b->held_count, b->held_host, b->held_own_used, b->held ? " (HELD now)" : "");
    n = strlen(out);
    if (n + 1 < size)
        snprintf(out + n, size - n, "; checkpoint mirror %s: passes sent %u got %u, taken here %u (calls %u), already had %u, dropped %u, other area %u%s", b->cfg.cp_mirror ? "ON" : "off",
                 b->cpm_sent, b->cpm_got, b->cpm_done, b->cpm_calls, b->cpm_had, b->cpm_dropped, b->cpm_other, b->cpm_in.have ? " (one waiting)" : "");
    n = strlen(out);
    if (n + 1 < size)
        snprintf(out + n, size - n, "; saved checkpoint here mID %d (%d / %d), the host's mID %d (%d / %d): stock follows refused %u, own Continues redirected %u", b->cp_saved.id, b->cp_saved.a,
                 b->cp_saved.b, b->host_cp.id, b->host_cp.a, b->host_cp.b, b->stock_refused, b->own_cont_redirected);
    n = strlen(out);
    if (n + 1 < size)
        snprintf(out + n, size - n, "; title save read %s: asked %u, done %u, failed %u, timed out %u, refused %u, skipped %u (state %d); start kind sent %d, waiting %d",
                 b->cfg.title_load ? "ON" : "off", b->tl_asked, b->tl_done, b->tl_failed, b->tl_timeout, b->tl_refused, b->tl_skipped, b->tl_state, b->tx_kind, b->rs_kind);
}
