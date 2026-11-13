/*
 * rdump1.c - fix021 LAB diagnostics (read only, never in a product decision): the resource table of sResource and the Campaign page's save panel entries.
 *  - rdump_table(filter): sResource [0x11E00FC] +0x4038 = 0x800 buckets x 4 slots of resource pointers (FUN_009B0170 / FUN_009B0280; a full probe chain
 *    gives up silently after 17 buckets): the used slots, and the resources whose first 0x60 bytes hold `filter` (their path), with refcount +0x48 and
 *    flags +0x50 (bit 4 failed, bit 6 cancelled).
 *  - rdump_panel(): the campaign slot entries [[0x116A328]+0x6C][0..2] (FUN_008B5CB0 kind 2): +0x30 / +0x34 status, +0x38 area, +0x40 difficulty,
 *    +0x48 play time (float s) - the values the title's Campaign page shows.
 *  - REV1_LAB_RDUMP=1: rdump_auto(why) logs both (the table with "em1000") at every barrier start (cb_start) and after every co-op write.
 * Console: `rtab [filter]`, `savepanel`.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "r1.h"

#define RD_SRES_PTR 0x11E00FCu
#define RD_TAB_OFF 0x4038u
#define RD_TAB_SLOTS 8192u
#define RD_SAVE_PTR 0x116A328u

static uint32_t rd32(uint32_t a)
{
    uint32_t v = 0;
    if (a < 0x10000u || !r1_safe_read((const void *)(uintptr_t)a, &v, 4)) return 0;
    return v;
}

static int rd_env = -1;
int rdump_on(void)
{
    if (rd_env < 0) {
        char v[8];
        rd_env = r1_env("REV1_LAB_RDUMP", v, sizeof v) && v[0] == '1';
    }
    return rd_env;
}

int rdump_table(const char *filter, char *out, int size)
{
    uint32_t sr = rd32(RD_SRES_PTR);
    if (!sr) return snprintf(out, (size_t)size, "rtab: no sResource");
    static uint32_t tab[RD_TAB_SLOTS];
    if (!r1_safe_read((const void *)(uintptr_t)(sr + RD_TAB_OFF), tab, sizeof tab)) return snprintf(out, (size_t)size, "rtab: the table is unreadable");
    unsigned used = 0, full_buckets = 0, shown = 0;
    for (unsigned b = 0; b < RD_TAB_SLOTS / 4; b++) {
        unsigned k = 0;
        for (unsigned s = 0; s < 4; s++) k += tab[b * 4 + s] != 0;
        used += k, full_buckets += k == 4;
    }
    int o = snprintf(out, (size_t)size, "rtab: sResource %08x table used %u of %u slots (%.1f%%), full buckets %u of %u", (unsigned)sr, used, RD_TAB_SLOTS, 100.0 * used / RD_TAB_SLOTS,
                     full_buckets, RD_TAB_SLOTS / 4);
    if (filter && filter[0]) {
        unsigned hits = 0;
        for (unsigned i = 0; i < RD_TAB_SLOTS && o < size - 200; i++) {
            uint32_t r = tab[i];
            char m[0x61];
            if (!r || !r1_safe_read((const void *)(uintptr_t)r, m, 0x60)) continue;
            m[0x60] = 0;
            const char *p = NULL;
            for (unsigned j = 4; j + strlen(filter) <= 0x60 && !p; j++)
                if (!memcmp(m + j, filter, strlen(filter))) p = m + j;
            if (!p) continue;
            hits++;
            if (shown++ < 8) {
                const char *st = p;   /* the start of the path: back to a non-printable byte */
                while (st > m + 4 && (unsigned char)st[-1] >= 0x20 && (unsigned char)st[-1] < 0x7F) st--;
                char path[0x60];
                snprintf(path, sizeof path, "%s", st);
                o += snprintf(out + o, (size_t)(size - o), "\n  slot %u res %08x vt %08x '%s' refcount %u flags %08x", i, (unsigned)r, (unsigned)rd32(r), path, (unsigned)rd32(r + 0x48),
                              (unsigned)rd32(r + 0x50));
            }
        }
        o += snprintf(out + o, (size_t)(size - o), "\n  '%s': %u resource(s) in the table", filter, hits);
    }
    return o;
}

int rdump_panel(char *out, int size)
{
    uint32_t sm = rd32(RD_SAVE_PTR), arr = sm ? rd32(sm + 0x6C) : 0;
    int o = snprintf(out, (size_t)size, "savepanel: sSaveManager %08x campaign entries %08x", (unsigned)sm, (unsigned)arr);
    for (unsigned s = 0; s < 3 && arr && o < size - 160; s++) {
        uint32_t e = rd32(arr + 4 * s);
        if (!e) {
            o += snprintf(out + o, (size_t)(size - o), " | slot %u: none", s);
            continue;
        }
        float pt = 0.0f;
        r1_safe_read((const void *)(uintptr_t)(e + 0x48), &pt, 4);
        o += snprintf(out + o, (size_t)(size - o), " | slot %u: entry %08x status %u/%u area %08x difficulty %d play %.0f s", s, (unsigned)e, (unsigned)rd32(e + 0x30), (unsigned)rd32(e + 0x34),
                      (unsigned)rd32(e + 0x38), (int)rd32(e + 0x40), (double)pt);
    }
    return o;
}

void rdump_auto(const char *why)
{
    if (!rdump_on()) return;
    static char b[2400];
    rdump_table("em1000", b, sizeof b);
    r1_log("rdump (%s): %s", why, b);
    rdump_panel(b, sizeof b);
    r1_log("rdump (%s): %s", why, b);
}
