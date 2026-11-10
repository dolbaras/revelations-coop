/*
 * mapptn1.c - the pure part of the minimap partner patch (see mapptn1.h). No Windows here: the offline tests link this file alone.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mapptn1.h"

const uint8_t mp1_site_orig[MP1_SITE_LEN] = {0x8b, 0x42, 0x24, 0x83, 0x78, 0x20, 0x01, 0x75, 0x41};
const uint8_t mp1_raid_path_bytes[6] = {0x8b, 0x0d, 0x94, 0x93, 0x16, 0x01};
const uint8_t mp1_no_partner_bytes[4] = {0x89, 0x5c, 0x24, 0x14};
const uint8_t mp1_continue_bytes[4] = {0x8b, 0x7c, 0x24, 0x1c};
const uint8_t mp1_partner_fn_bytes[6] = {0x53, 0x55, 0x8b, 0xe9, 0x8b, 0x5d};

const char *mp1_state_name(int st) { return st == MP1_OFF ? "off" : st == MP1_ON ? "ON" : st == MP1_MISMATCH ? "REFUSED (bytes differ)" : "ERROR"; }

static void logf_(const mp1_mem *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const mp1_mem *m, const char *fmt, ...)
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

void mp1_targets_game(mp1_targets *t)
{
    t->raid_path = MP1_RAID_PATH, t->no_partner = MP1_NO_PARTNER, t->cont = MP1_CONTINUE, t->partner_fn = MP1_PARTNER_FN, t->splayer_cell = MP1_SPLAYER;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t rel(uint32_t from_next, uint32_t to) { return to - from_next; }

/*  0  8b 42 24              mov eax,[edx+0x24]
 *  3  83 78 20 01           cmp dword ptr [eax+0x20],1
 *  7  0f 84 rel32           je raid_path                 (Raid: the stock partner block, AI hide included)
 * 13  8b 0d <sPlayer>       mov ecx,[sPlayer cell]
 * 19  85 c9                 test ecx,ecx
 * 21  0f 84 rel32           je no_partner                (no sPlayer: the stock "no partner")
 * 27  8d 54 24 13           lea edx,[esp+0x13]           (the out byte of the stock call)
 * 31  52                    push edx
 * 32  e8 rel32              call partner_fn              (thiscall, ret 4)
 * 37  89 44 24 14           mov [esp+0x14],eax           (vf9's partner slot)
 * 41  e9 rel32              jmp cont
 * 46 */
int mp1_stub_build(uint8_t *out, uint32_t stub, const mp1_targets *t)
{
    static const uint8_t head[7] = {0x8b, 0x42, 0x24, 0x83, 0x78, 0x20, 0x01};
    memcpy(out, head, 7);
    out[7] = 0x0f, out[8] = 0x84, put32(out + 9, rel(stub + 13, t->raid_path));
    out[13] = 0x8b, out[14] = 0x0d, put32(out + 15, t->splayer_cell);
    out[19] = 0x85, out[20] = 0xc9;
    out[21] = 0x0f, out[22] = 0x84, put32(out + 23, rel(stub + 27, t->no_partner));
    out[27] = 0x8d, out[28] = 0x54, out[29] = 0x24, out[30] = 0x13;
    out[31] = 0x52;
    out[32] = 0xe8, put32(out + 33, rel(stub + 37, t->partner_fn));
    out[37] = 0x89, out[38] = 0x44, out[39] = 0x24, out[40] = 0x14;
    out[41] = 0xe9, put32(out + 42, rel(stub + 46, t->cont));
    return MP1_STUB_LEN;
}

void mp1_site_build_at(uint8_t out[MP1_SITE_LEN], uint32_t site, uint32_t stub)
{
    out[0] = 0xe9;
    put32(out + 1, rel(site + 5, stub));
    memset(out + 5, 0x90, MP1_SITE_LEN - 5);
}

void mp1_site_build(uint8_t out[MP1_SITE_LEN], uint32_t stub) { mp1_site_build_at(out, MP1_SITE, stub); }

void mp1_init(mp1_t *g, int env)
{
    memset(g, 0, sizeof *g);
    g->env = env;
}

int mp1_want(const mp1_t *g, int split_on, int net_active)
{
    if (g->mode == MP1_MODE_OFF)
        return 0;
    if (g->mode == MP1_MODE_ON)
        return 1;
    if (g->env == 0)
        return 0;
    if (g->env == 1)
        return 1;
    return split_on || net_active;
}

static void refuse(mp1_t *g, const mp1_mem *m, int st, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void refuse(mp1_t *g, const mp1_mem *m, int st, const char *fmt, ...)
{
    char why[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    int first = g->state != st || strcmp(g->why, why) != 0;   /* asked again every frame by the glue: logged and counted once */
    g->state = st;
    snprintf(g->why, sizeof g->why, "%s", why);
    if (first) {
        g->refusals++;
        logf_(m, "mapptn: minimap partner patch not written: %s", why);
    }
}

static int expect(const mp1_mem *m, uint32_t a, const uint8_t *want, uint32_t n)
{
    uint8_t cur[16];
    return n <= sizeof cur && m->read(m->user, a, cur, n) && memcmp(cur, want, n) == 0;
}

static int stub_ready(mp1_t *g, const mp1_mem *m)
{
    if (g->stub)
        return 1;
    uint32_t blk = m->alloc ? m->alloc(m->user, 64) : 0;
    if (!blk) {
        refuse(g, m, MP1_ERROR, "no executable memory for the stub");
        return 0;
    }
    uint8_t code[MP1_STUB_LEN];
    mp1_targets t;
    mp1_targets_game(&t);
    int n = mp1_stub_build(code, blk, &t);
    if (!m->write(m->user, blk, code, (uint32_t)n)) {
        refuse(g, m, MP1_ERROR, "cannot fill the stub");
        return 0;
    }
    g->stub = blk;
    return 1;
}

int mp1_apply(mp1_t *g, const mp1_mem *m)
{
    uint8_t cur[MP1_SITE_LEN];
    if (!m->read(m->user, MP1_SITE, cur, sizeof cur)) {
        refuse(g, m, MP1_ERROR, "cannot read %08x", MP1_SITE);
        return 0;
    }
    if (g->stub) {
        uint8_t ours[MP1_SITE_LEN];
        mp1_site_build(ours, g->stub);
        if (memcmp(cur, ours, sizeof cur) == 0) {   /* ours already */
            g->state = MP1_ON;
            return 1;
        }
    }
    if (memcmp(cur, mp1_site_orig, sizeof cur) != 0) {
        char hx[48];
        hex_(hx, sizeof hx, cur, MP1_SITE_LEN);
        refuse(g, m, MP1_MISMATCH, "bytes at %08x are %s", MP1_SITE, hx);
        return 0;
    }
    /* every place the stub jumps to or calls must be the code it expects (the build check of the whole path, not only the 9 bytes) */
    if (!expect(m, MP1_RAID_PATH, mp1_raid_path_bytes, 6) || !expect(m, MP1_NO_PARTNER, mp1_no_partner_bytes, 4) || !expect(m, MP1_CONTINUE, mp1_continue_bytes, 4) ||
        !expect(m, MP1_PARTNER_FN, mp1_partner_fn_bytes, 6)) {
        refuse(g, m, MP1_MISMATCH, "a jump target of the stub is not the expected code (another build?)");
        return 0;
    }
    if (!stub_ready(g, m))
        return 0;
    uint8_t site[MP1_SITE_LEN];
    mp1_site_build(site, g->stub);
    if (!m->write(m->user, MP1_SITE, site, MP1_SITE_LEN)) {
        refuse(g, m, MP1_ERROR, "write to %08x failed", MP1_SITE);
        return 0;
    }
    g->state = MP1_ON, g->why[0] = 0, g->applies++;
    logf_(m, "mapptn: minimap partner patch written: %08x jmp %08x (campaign: the partner of FUN_0041BC10 goes onto the HUD minimap, Raid unchanged)", MP1_SITE, (unsigned)g->stub);
    return 1;
}

int mp1_revert(mp1_t *g, const mp1_mem *m)
{
    uint8_t cur[MP1_SITE_LEN], ours[MP1_SITE_LEN];
    if (!m->read(m->user, MP1_SITE, cur, sizeof cur))
        return 0;
    if (memcmp(cur, mp1_site_orig, sizeof cur) == 0) {
        g->state = MP1_OFF;
        return 1;
    }
    if (!g->stub) {
        refuse(g, m, MP1_MISMATCH, "revert: the bytes are not ours, left alone");
        return 0;
    }
    mp1_site_build(ours, g->stub);
    if (memcmp(cur, ours, sizeof cur) != 0) {
        refuse(g, m, MP1_MISMATCH, "revert: the bytes are not ours, left alone");
        return 0;
    }
    if (!m->write(m->user, MP1_SITE, mp1_site_orig, MP1_SITE_LEN))
        return 0;
    g->state = MP1_OFF, g->why[0] = 0, g->reverts++;
    logf_(m, "mapptn: minimap partner patch removed (%08x original again: the stock campaign minimap without a partner)", MP1_SITE);
    return 1;
}

int mp1_probe(const mp1_t *g, const mp1_mem *m, char *out, size_t size)
{
    uint8_t cur[MP1_SITE_LEN];
    if (!m->read(m->user, MP1_SITE, cur, sizeof cur)) {
        snprintf(out, size, "unreadable");
        return MP1_ERROR;
    }
    if (memcmp(cur, mp1_site_orig, sizeof cur) == 0) {
        snprintf(out, size, "original");
        return MP1_OFF;
    }
    if (g->stub) {
        uint8_t ours[MP1_SITE_LEN];
        mp1_site_build(ours, g->stub);
        if (memcmp(cur, ours, sizeof cur) == 0) {
            snprintf(out, size, "patched (jmp %08x)", (unsigned)g->stub);
            return MP1_ON;
        }
    }
    char hx[48];
    hex_(hx, sizeof hx, cur, MP1_SITE_LEN);
    snprintf(out, size, "FOREIGN %s", hx);
    return MP1_MISMATCH;
}

void mp1_status(const mp1_t *g, const mp1_mem *m, int split_on, int net_active, char *out, size_t size)
{
    static const char *const mode[] = {"auto", "forced on", "forced off"};
    char live[64];
    mp1_probe(g, m, live, sizeof live);
    snprintf(out, size, "mapptn: mode %s, REV1_MAPPTN %d, split %s, network session %s, wanted %s; site %08x state %s, live bytes %s; applied %u, reverted %u, refused %u%s%s",
             mode[g->mode >= 0 && g->mode < 3 ? g->mode : 0], g->env, split_on ? "ON" : "no", net_active ? "ACTIVE" : "no", mp1_want(g, split_on, net_active) ? "on" : "off", MP1_SITE,
             mp1_state_name(g->state), live, g->applies, g->reverts, g->refusals, g->why[0] ? ", why: " : "", g->why);
}
