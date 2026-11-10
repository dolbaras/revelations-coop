/*
 * guards1.c - the pure part of the network guards (see guards1.h). No Windows here: the offline tests link this file alone.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "guards1.h"

const uint8_t g1_exit_orig[12] = {0x85, 0xff, 0x74, 0x08, 0x81, 0xff, 0xcd, 0xcd, 0xcd, 0xcd, 0x75, 0x18};
const uint8_t g1_pick_orig[G1_PICK_CHECK] = {0x51, 0x56, 0x8b, 0xf1, 0xe8, 0x47, 0x55, 0x14, 0x00};
const uint8_t g1_exit_target_bytes[3] = {0x85, 0xff, 0x74};

const char *g1_name(int which) { return which == G1_EXIT ? "exit" : "pick"; }
const char *g1_state_name(int st) { return st == G1_OFF ? "off" : st == G1_ON ? "ON" : st == G1_MISMATCH ? "REFUSED (bytes differ)" : "ERROR"; }

static void logf_(const g1_mem *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const g1_mem *m, const char *fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (m && m->log)
        m->log(m->user, line);
}

static void hex_(char *out, size_t size, const uint8_t *b, int n)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 4 < size; i++)
        o += (size_t)snprintf(out + o, size - o, "%s%02x", i ? " " : "", b[i]);
    if (size && o >= size)
        out[size - 1] = 0;
}

/* mov eax,[esp+4] ; cmp eax,[cell] ; sete al ; movzx eax,al ; ret 4  (thiscall with one stack argument, the slot; eax is the result, ecx is not touched) */
int g1_stub_build(uint8_t *out, uint32_t cell)
{
    static const uint8_t head[] = {0x8B, 0x44, 0x24, 0x04, 0x3B, 0x05};
    static const uint8_t tail[] = {0x0F, 0x94, 0xC0, 0x0F, 0xB6, 0xC0, 0xC2, 0x04, 0x00};
    memcpy(out, head, sizeof head);
    memcpy(out + sizeof head, &cell, 4);
    memcpy(out + sizeof head + 4, tail, sizeof tail);
    return (int)(sizeof head + 4 + sizeof tail);
}

void g1_jmp_build(uint8_t out[5], uint32_t from, uint32_t to)
{
    uint32_t rel = to - (from + 5);
    out[0] = 0xE9;
    memcpy(out + 1, &rel, 4);
}

void g1_init(g1_t *g, int env_on)
{
    memset(g, 0, sizeof *g);
    g->env_on = env_on != 0;
}

int g1_want(const g1_t *g, int net_active)
{
    if (g->mode == G1_MODE_OFF)
        return 0;
    if (g->mode == G1_MODE_ON)
        return 1;
    return g->env_on || net_active;
}

static void refuse(g1_t *g, const g1_mem *m, int which, int st, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void refuse(g1_t *g, const g1_mem *m, int which, int st, const char *fmt, ...)
{
    char why[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    int first = g->state[which] != st || strcmp(g->why[which], why) != 0;   /* the glue may ask again; a refusal is logged and counted once */
    g->state[which] = st;
    snprintf(g->why[which], sizeof g->why[which], "%s", why);
    if (first) {
        g->refusals[which]++;
        logf_(m, "guards: %s patch not written: %s", g1_name(which), why);
    }
}

/* ---- the kill-record assert ---- */
static int exit_apply(g1_t *g, const g1_mem *m)
{
    uint8_t cur[12], ours[12], tgt[3];
    memcpy(ours, g1_exit_orig, sizeof ours);
    ours[3] = 0x20;
    if (!m->read(m->user, G1_EXIT_ADDR, cur, sizeof cur)) {
        refuse(g, m, G1_EXIT, G1_ERROR, "cannot read %08x", G1_EXIT_ADDR);
        return 0;
    }
    if (memcmp(cur, ours, sizeof cur) == 0) {   /* ours already */
        g->state[G1_EXIT] = G1_ON;
        return 1;
    }
    if (memcmp(cur, g1_exit_orig, sizeof cur) != 0) {
        char hx[64];
        hex_(hx, sizeof hx, cur, 12);
        refuse(g, m, G1_EXIT, G1_MISMATCH, "bytes at %08x are %s", G1_EXIT_ADDR, hx);
        return 0;
    }
    if (!m->read(m->user, G1_EXIT_TARGET, tgt, sizeof tgt) || memcmp(tgt, g1_exit_target_bytes, sizeof tgt) != 0) {   /* the jump must land on `test edi,edi / jz` */
        refuse(g, m, G1_EXIT, G1_MISMATCH, "the jump target %08x is not `test edi,edi / jz`", G1_EXIT_TARGET);
        return 0;
    }
    const uint8_t v = 0x20;
    if (!m->write(m->user, G1_EXIT_PATCH, &v, 1)) {
        refuse(g, m, G1_EXIT, G1_ERROR, "write to %08x failed", G1_EXIT_PATCH);
        return 0;
    }
    g->state[G1_EXIT] = G1_ON, g->why[G1_EXIT][0] = 0, g->applies[G1_EXIT]++;
    logf_(m, "guards: exit patch written: %08x 08 -> 20 (a kill record without a knife skips the assert)", G1_EXIT_PATCH);
    return 1;
}

static int exit_revert(g1_t *g, const g1_mem *m)
{
    uint8_t cur[12], ours[12];
    memcpy(ours, g1_exit_orig, sizeof ours);
    ours[3] = 0x20;
    if (!m->read(m->user, G1_EXIT_ADDR, cur, sizeof cur))
        return 0;
    if (memcmp(cur, g1_exit_orig, sizeof cur) == 0) {   /* already original */
        g->state[G1_EXIT] = G1_OFF;
        return 1;
    }
    if (memcmp(cur, ours, sizeof cur) != 0) {   /* somebody else changed it: do not touch */
        refuse(g, m, G1_EXIT, G1_MISMATCH, "revert: the bytes are not ours, left alone");
        return 0;
    }
    const uint8_t v = 0x08;
    if (!m->write(m->user, G1_EXIT_PATCH, &v, 1))
        return 0;
    g->state[G1_EXIT] = G1_OFF, g->why[G1_EXIT][0] = 0, g->reverts[G1_EXIT]++;
    logf_(m, "guards: exit patch removed (%08x back to 08)", G1_EXIT_PATCH);
    return 1;
}

/* ---- the pickup condition ---- */
static int pick_stub_ready(g1_t *g, const g1_mem *m, uint32_t own)
{
    if (g->stub)
        return 1;
    uint32_t blk = m->alloc ? m->alloc(m->user, 64) : 0;
    if (!blk) {
        refuse(g, m, G1_PICK, G1_ERROR, "no executable memory for the stub");
        return 0;
    }
    uint8_t code[32];
    uint32_t cell = blk + 32;
    int n = g1_stub_build(code, cell);
    if (!m->write(m->user, blk, code, (uint32_t)n) || !m->write(m->user, cell, &own, 4)) {
        refuse(g, m, G1_PICK, G1_ERROR, "cannot fill the stub");
        return 0;
    }
    g->stub = blk, g->cell = cell, g->own = own;
    return 1;
}

static int pick_apply(g1_t *g, const g1_mem *m, uint32_t own)
{
    uint8_t cur[G1_PICK_CHECK];
    if (!m->read(m->user, G1_PICK_ADDR, cur, sizeof cur)) {
        refuse(g, m, G1_PICK, G1_ERROR, "cannot read %08x", G1_PICK_ADDR);
        return 0;
    }
    if (g->stub && cur[0] == 0xE9) {
        uint8_t want[5];
        g1_jmp_build(want, G1_PICK_ADDR, g->stub);
        if (memcmp(cur, want, 5) == 0 && memcmp(cur + 5, g1_pick_orig + 5, 4) == 0) {   /* ours already */
            g->state[G1_PICK] = G1_ON;
            return 1;
        }
    }
    if (memcmp(cur, g1_pick_orig, sizeof cur) != 0) {
        char hx[64];
        hex_(hx, sizeof hx, cur, G1_PICK_CHECK);
        refuse(g, m, G1_PICK, G1_MISMATCH, "bytes at %08x are %s", G1_PICK_ADDR, hx);
        return 0;
    }
    if (!pick_stub_ready(g, m, own))
        return 0;
    if (!m->write(m->user, g->cell, &own, 4)) {   /* the cell first, then the jump */
        refuse(g, m, G1_PICK, G1_ERROR, "cannot write the own slot cell");
        return 0;
    }
    g->own = own;
    uint8_t jmp[5];
    g1_jmp_build(jmp, G1_PICK_ADDR, g->stub);
    if (!m->write(m->user, G1_PICK_ADDR, jmp, 5)) {
        refuse(g, m, G1_PICK, G1_ERROR, "write to %08x failed", G1_PICK_ADDR);
        return 0;
    }
    g->state[G1_PICK] = G1_ON, g->why[G1_PICK][0] = 0, g->applies[G1_PICK]++;
    logf_(m, "guards: pickup patch written: %08x jmp %08x (slot == own slot %u instead of slot == 0)", G1_PICK_ADDR, g->stub, (unsigned)own);
    return 1;
}

static int pick_revert(g1_t *g, const g1_mem *m)
{
    uint8_t cur[G1_PICK_CHECK], ours[G1_PICK_CHECK];
    if (!m->read(m->user, G1_PICK_ADDR, cur, sizeof cur))
        return 0;
    if (memcmp(cur, g1_pick_orig, sizeof cur) == 0) {
        g->state[G1_PICK] = G1_OFF;
        return 1;
    }
    memcpy(ours, g1_pick_orig, sizeof ours);
    if (g->stub)
        g1_jmp_build(ours, G1_PICK_ADDR, g->stub);
    if (!g->stub || memcmp(cur, ours, sizeof cur) != 0) {
        refuse(g, m, G1_PICK, G1_MISMATCH, "revert: the bytes are not ours, left alone");
        return 0;
    }
    if (!m->write(m->user, G1_PICK_ADDR, g1_pick_orig, 5))
        return 0;
    g->state[G1_PICK] = G1_OFF, g->why[G1_PICK][0] = 0, g->reverts[G1_PICK]++;
    logf_(m, "guards: pickup patch removed (%08x original again)", G1_PICK_ADDR);
    return 1;
}

int g1_apply(g1_t *g, const g1_mem *m, int which, uint32_t own)
{
    return which == G1_EXIT ? exit_apply(g, m) : pick_apply(g, m, own);
}

int g1_revert(g1_t *g, const g1_mem *m, int which)
{
    return which == G1_EXIT ? exit_revert(g, m) : pick_revert(g, m);
}

void g1_set_own(g1_t *g, const g1_mem *m, uint32_t own)
{
    if (g->state[G1_PICK] != G1_ON || !g->cell || own == g->own)
        return;
    if (m->write(m->user, g->cell, &own, 4)) {
        logf_(m, "guards: pickup own slot %u -> %u", (unsigned)g->own, (unsigned)own);
        g->own = own;
    }
}

int g1_probe(const g1_t *g, const g1_mem *m, int which, char *out, size_t size)
{
    uint8_t cur[12];
    int n = which == G1_EXIT ? 12 : G1_PICK_CHECK;
    uint32_t a = which == G1_EXIT ? G1_EXIT_ADDR : G1_PICK_ADDR;
    if (!m->read(m->user, a, cur, (uint32_t)n)) {
        snprintf(out, size, "unreadable");
        return G1_ERROR;
    }
    if (which == G1_EXIT) {
        uint8_t ours[12];
        memcpy(ours, g1_exit_orig, 12);
        ours[3] = 0x20;
        if (memcmp(cur, g1_exit_orig, 12) == 0) {
            snprintf(out, size, "original");
            return G1_OFF;
        }
        if (memcmp(cur, ours, 12) == 0) {
            snprintf(out, size, "patched (byte %08x = 20)", G1_EXIT_PATCH);
            return G1_ON;
        }
    } else {
        uint8_t jmp[5];
        if (memcmp(cur, g1_pick_orig, (size_t)n) == 0) {
            snprintf(out, size, "original");
            return G1_OFF;
        }
        if (g->stub) {
            g1_jmp_build(jmp, G1_PICK_ADDR, g->stub);
            if (memcmp(cur, jmp, 5) == 0 && memcmp(cur + 5, g1_pick_orig + 5, 4) == 0) {
                snprintf(out, size, "patched (jmp %08x)", (unsigned)g->stub);
                return G1_ON;
            }
        }
    }
    char hx[64];
    hex_(hx, sizeof hx, cur, n);
    snprintf(out, size, "FOREIGN %s", hx);
    return G1_MISMATCH;
}

void g1_status(const g1_t *g, const g1_mem *m, int net_active, uint32_t own, char *out, size_t size)
{
    static const char *const mode[] = {"auto", "forced on", "forced off"};
    size_t o = 0;
    o += (size_t)snprintf(out + o, size - o, "guards: mode %s, REV1_GUARDS %d, network session %s, wanted %s, own slot %u (cell %s)", mode[g->mode >= 0 && g->mode < 3 ? g->mode : 0], g->env_on,
                          net_active ? "ACTIVE" : "no", g1_want(g, net_active) ? "on" : "off", (unsigned)own, g->stub ? (g->own == own ? "in step" : "BEHIND") : "none");
    for (int w = 0; w < G1_N && o < size; w++) {
        char live[96];
        g1_probe(g, m, w, live, sizeof live);
        o += (size_t)snprintf(out + o, size - o, "\n  %-4s %s at %08x: state %s, live bytes %s; applied %u, reverted %u, refused %u%s%s", g1_name(w), w == G1_EXIT ? "(exe+1396 assert)" : "(pickup slot==own)",
                              w == G1_EXIT ? G1_EXIT_PATCH : G1_PICK_ADDR, g1_state_name(g->state[w]), live, g->applies[w], g->reverts[w], g->refusals[w], g->why[w][0] ? ", why: " : "", g->why[w]);
    }
}
