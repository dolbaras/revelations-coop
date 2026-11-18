/*
 * aimray1.c - the pure part of the second human's aim ray (see aimray1.h). No Windows here: tests/test_aimray1.c links this file alone.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "aimray1.h"

const uint8_t ar1_site_orig[AR1_SITE_LEN] = {0xA1, 0xB8, 0x00, 0x1E, 0x01};

const ar1_site ar1_sites[AR1_N_SITES] = {
    {0x5EBC16u, AR1_REG_ECX, {0x81, 0xEC, 0x24, 0x01, 0x00, 0x00, 0x53, 0x56}, 0x5EE409u, {0x8B, 0xCE, 0xF3, 0x0F, 0x11, 0x44, 0x24}, 7,
     "FUN_005EBC10 the view / projection of the aim ray (manager +0xCE0, rect)"},
    {0x5EE3BBu, AR1_REG_ESI, {0x8B, 0x50, 0x48, 0x8B, 0x48, 0x50, 0x8B, 0x78}, 0x5EE35Du, {0x8B, 0xF1}, 2, "FUN_005EE350 the camera ray (the rect's half size)"},
    {0x5EFB1Fu, AR1_REG_ESI, {0x8B, 0x48, 0x48, 0x8B, 0x40, 0x4C, 0x0F, 0x57}, 0x5EFAC1u, {0x8B, 0xF1}, 2, "FUN_005EFAC0 the aim's screen point"},
};

/* FUN_005EE350 is FUN_005EBC10's only caller: `call 0x5EBC10` right after the `mov ecx, esi` above */
static const uint8_t call_ebc10[5] = {0xE8, 0xFA, 0xD7, 0xFF, 0xFF};
#define AR1_CALL_EBC10 0x5EE411u

ar1_t ar1_g;

const char *ar1_state_name(int st) { return st == AR1_OFF ? "off" : st == AR1_ON ? "ON" : st == AR1_MISMATCH ? "REFUSED (bytes differ)" : "ERROR"; }

static void logf_(const ar1_mem *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const ar1_mem *m, const char *fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (m && m->log)
        m->log(m->user, line);
}

void ar1_init(ar1_t *g, uint32_t cam_cell, int enabled)
{
    memset(g, 0, sizeof *g);
    g->cam_cell = cam_cell, g->enabled = enabled;
}

void ar1_set_view(ar1_t *g, int on, uint32_t mgr1, int own, int w, int h)
{
    if (!on || !mgr1 || w <= 0 || h <= 0) {
        g->snap.on = 0;
        return;
    }
    if (g->snap.mgr1 != mgr1 || g->snap.own != (uint32_t)own || g->snap.w != w || g->snap.h != h)
        g->snap.on = 0;   /* (the fields change while no reader takes them as a whole: off first) */
    g->snap.mgr1 = mgr1, g->snap.own = (uint32_t)own, g->snap.w = w, g->snap.h = h;
    g->snap.on = 1;
}

int ar1_second_view(const ar1_snap *s, int enabled, uint32_t real, uint32_t real_mgr1, uint32_t owner, uint32_t kind, uint32_t slot)
{
    if (!enabled || !s->on || !real || !owner || kind != 1u || slot > 1u || slot == s->own)
        return 0;
    return s->mgr1 != 0 && real_mgr1 == s->mgr1;   /* the camera root still holds the manager the split put on viewport 2 */
}

static uint32_t rd(uint32_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }

/* the stubs' call: the weapon is the one the stock function works on (it dereferenced the same fields right before) */
__attribute__((force_align_arg_pointer)) uint32_t ar1_choose(uint32_t weapon)
{
    ar1_t *g = &ar1_g;
    uint32_t real = rd(g->cam_cell);
    g->n_calls++;
    if (!g->enabled || !g->snap.on || !real || !weapon)
        return real;
    uint32_t owner = rd(weapon + AR1_W_OWNER);
    if (!owner) {
        uint32_t h = rd(weapon + AR1_W_HOLDER);
        owner = h ? rd(h + AR1_HOLDER_OWNER) : 0;
    }
    if (!owner)
        return real;
    uint32_t kind = rd(owner + AR1_U_KIND);
    if (kind != 1u)
        return real;
    if (!ar1_second_view(&g->snap, g->enabled, real, rd(real + AR1_OFF_MGR1), owner, kind, rd(owner + AR1_U_SLOT)))
        return real;
    uint32_t *f = g->fake;   /* every thread writes the same values (one second view): no lock needed */
    f[AR1_OFF_RECT / 4] = 0, f[AR1_OFF_RECT / 4 + 1] = 0;
    f[AR1_OFF_RECT / 4 + 2] = (uint32_t)g->snap.w, f[AR1_OFF_RECT / 4 + 3] = (uint32_t)g->snap.h;
    f[AR1_OFF_MGR0 / 4] = g->snap.mgr1, f[AR1_OFF_MGR1 / 4] = g->snap.mgr1;
    g->n_second++;
    g->last_weapon = weapon, g->last_owner = owner;
    return (uint32_t)(uintptr_t)f;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/*   0  9c                   pushfd
 *   1  51                   push ecx
 *   2  52                   push edx
 *   3  81 ec 80 00 00 00    sub esp, 0x80
 *   9  0f 11 04 24          movups [esp], xmm0
 *  13  0f 11 4c 24 10 ..    movups [esp+0x10*k], xmm1..7 (5 bytes each)
 *  48  51 | 56              push ecx | push esi      (the weapon)
 *  49  e8 rel32             call ar1_choose (cdecl: eax = the pointer the stock `mov eax,[0x11E00B8]` loads, or the stand-in)
 *  54  83 c4 04             add esp, 4
 *  57  0f 10 04 24          movups xmm0, [esp]
 *  61  0f 10 4c 24 10 ..    movups xmm1..7, [esp+0x10*k]
 *  96  81 c4 80 00 00 00    add esp, 0x80
 * 102  5a 59 9d c3          pop edx; pop ecx; popfd; ret       (106 bytes, padded with int3 to AR1_STUB_LEN) */
int ar1_stub_build(uint8_t *out, uint32_t stub, int reg, uint32_t fn)
{
    int o = 0;
    out[o++] = 0x9C, out[o++] = 0x51, out[o++] = 0x52;
    out[o++] = 0x81, out[o++] = 0xEC, put32(out + o, 0x80), o += 4;
    out[o++] = 0x0F, out[o++] = 0x11, out[o++] = 0x04, out[o++] = 0x24;
    for (int k = 1; k < 8; k++)
        out[o++] = 0x0F, out[o++] = 0x11, out[o++] = (uint8_t)(0x44 | (k << 3)), out[o++] = 0x24, out[o++] = (uint8_t)(0x10 * k);
    out[o++] = reg == AR1_REG_ESI ? 0x56 : 0x51;
    out[o] = 0xE8, put32(out + o + 1, fn - (stub + (uint32_t)o + 5)), o += 5;
    out[o++] = 0x83, out[o++] = 0xC4, out[o++] = 0x04;
    out[o++] = 0x0F, out[o++] = 0x10, out[o++] = 0x04, out[o++] = 0x24;
    for (int k = 1; k < 8; k++)
        out[o++] = 0x0F, out[o++] = 0x10, out[o++] = (uint8_t)(0x44 | (k << 3)), out[o++] = 0x24, out[o++] = (uint8_t)(0x10 * k);
    out[o++] = 0x81, out[o++] = 0xC4, put32(out + o, 0x80), o += 4;
    out[o++] = 0x5A, out[o++] = 0x59, out[o++] = 0x9D, out[o++] = 0xC3;
    while (o < AR1_STUB_LEN)
        out[o++] = 0xCC;
    return o;
}

void ar1_site_build(uint8_t out[AR1_SITE_LEN], uint32_t site, uint32_t stub)
{
    out[0] = 0xE8;
    put32(out + 1, stub - (site + 5));
}

static void refuse(ar1_t *g, const ar1_mem *m, int st, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void refuse(ar1_t *g, const ar1_mem *m, int st, const char *fmt, ...)
{
    char why[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    int first = g->state != st || strcmp(g->why, why) != 0;
    g->state = st;
    snprintf(g->why, sizeof g->why, "%s", why);
    if (first) {
        g->refusals++;
        logf_(m, "split: aim ray of the second human not written: %s", why);
    }
}

static int expect(const ar1_mem *m, uint32_t a, const uint8_t *want, uint32_t n)
{
    uint8_t cur[16];
    return n <= sizeof cur && m->read(m->user, a, cur, n) && memcmp(cur, want, n) == 0;
}

int ar1_apply(ar1_t *g, const ar1_mem *m, uint32_t fn)
{
    uint8_t cur[AR1_SITE_LEN], ours[AR1_SITE_LEN];
    int n_ours = 0;
    for (int i = 0; i < AR1_N_SITES; i++) {
        const ar1_site *s = &ar1_sites[i];
        if (!m->read(m->user, s->addr, cur, sizeof cur)) {
            refuse(g, m, AR1_ERROR, "cannot read %08x", s->addr);
            return 0;
        }
        if (g->stub[i]) {
            ar1_site_build(ours, s->addr, g->stub[i]);
            if (!memcmp(cur, ours, sizeof cur)) {
                n_ours++;
                continue;
            }
        }
        if (memcmp(cur, ar1_site_orig, sizeof cur) != 0 || !expect(m, s->addr + AR1_SITE_LEN, s->next, sizeof s->next) ||
            !expect(m, s->reg_addr, s->reg_bytes, (uint32_t)s->reg_len)) {
            refuse(g, m, AR1_MISMATCH, "the bytes at or around %08x are not those of build 13298487", s->addr);
            return 0;
        }
    }
    if (n_ours == AR1_N_SITES) {
        g->state = AR1_ON;
        return 1;
    }
    if (n_ours) {
        refuse(g, m, AR1_MISMATCH, "only %d of %d sites are ours", n_ours, AR1_N_SITES);
        return 0;
    }
    if (!expect(m, AR1_CALL_EBC10, call_ebc10, sizeof call_ebc10)) {
        refuse(g, m, AR1_MISMATCH, "FUN_005EE350 does not call FUN_005EBC10 at %08x", AR1_CALL_EBC10);
        return 0;
    }
    for (int i = 0; i < AR1_N_SITES; i++) {
        if (g->stub[i])
            continue;
        uint32_t blk = m->alloc ? m->alloc(m->user, AR1_STUB_LEN) : 0;
        uint8_t code[AR1_STUB_LEN];
        if (!blk) {
            refuse(g, m, AR1_ERROR, "no executable memory for the stubs");
            return 0;
        }
        ar1_stub_build(code, blk, ar1_sites[i].reg, fn);
        if (!m->write(m->user, blk, code, AR1_STUB_LEN)) {
            refuse(g, m, AR1_ERROR, "cannot fill a stub");
            return 0;
        }
        g->stub[i] = blk;
    }
    for (int i = 0; i < AR1_N_SITES; i++) {
        ar1_site_build(ours, ar1_sites[i].addr, g->stub[i]);
        if (!m->write(m->user, ar1_sites[i].addr, ours, AR1_SITE_LEN)) {
            for (int k = 0; k < i; k++)
                m->write(m->user, ar1_sites[k].addr, ar1_site_orig, AR1_SITE_LEN);
            refuse(g, m, AR1_ERROR, "write to %08x failed (the earlier sites put back)", ar1_sites[i].addr);
            return 0;
        }
    }
    g->state = AR1_ON, g->why[0] = 0, g->applies++;
    logf_(m, "split: aim ray of the second human written: %08x / %08x / %08x call their stubs (the second human's shots follow his own view)", ar1_sites[0].addr,
          ar1_sites[1].addr, ar1_sites[2].addr);
    return 1;
}

int ar1_revert(ar1_t *g, const ar1_mem *m)
{
    uint8_t cur[AR1_SITE_LEN], ours[AR1_SITE_LEN];
    int bad = 0;
    for (int i = 0; i < AR1_N_SITES; i++) {
        if (!m->read(m->user, ar1_sites[i].addr, cur, sizeof cur))
            return 0;
        if (!memcmp(cur, ar1_site_orig, sizeof cur))
            continue;
        if (g->stub[i]) {
            ar1_site_build(ours, ar1_sites[i].addr, g->stub[i]);
            if (!memcmp(cur, ours, sizeof cur)) {
                m->write(m->user, ar1_sites[i].addr, ar1_site_orig, AR1_SITE_LEN);
                continue;
            }
        }
        bad++;
    }
    if (bad) {
        refuse(g, m, AR1_MISMATCH, "revert: %d site(s) hold bytes that are not ours, left alone", bad);
        return 0;
    }
    g->state = AR1_OFF, g->reverts++;
    return 1;
}
