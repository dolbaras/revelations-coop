/*
 * zone1_game.inc - included by world1_game.c (uses its statics): the engine side of the puppet-authority rule for the story's event zones (zone1.h has the
 * rule and the evidence). One detour: cEventCollision::HitCheck 0x6D5980 (thiscall(this; record) ret 4, al = hit). Only the two FSM call sites count
 * (return addresses below, checked in the image by tests/test_world1_game.c); every other caller (doors, lifts, grenades) goes through untouched.
 *
 * Threads: the hook runs on the thread of the FSM update. It reads only the game structures it was handed (this, its resource, the record, the hitter slots)
 * and two unit pointers the logic pass caches (compared, never followed); when a report counts it looks the puppet up again (players list) before it writes it
 * into a slot. The table (zone1.h) is shared with the logic pass under zcs; nothing of the game is called while zcs is held.
 *
 * `world zone [status] | on | off | reset` (default ON while the model is bound; REV1_ZONE=0 starts it off).
 * LAB probe (round 9 task 3): `world zone watch <record[,record..]> [seconds] [path part[$]]` logs every FSM test of those records (< 32; any path, or paths
 * containing the part; a trailing $ = the path ends with it):
 * the stock result, the hitters, the own human's position, area blocks (unit+0xE88 +0x298 / +0x2A4, FUN_006EDF70) and facing (quaternion +0x50) against the
 * record's block pairs (+0x90 / +0x9C, gate when (+0xB8 & ~2) == 1), facing gate (+0xA8, +0xAC, +0xB0..+0xB4), centre and radius. `world zone watch off`.
 */
#include "zone1.h"

#define Z_RA_HITCHECK 0x74BCB1u   /* EventCollisionHitCheck 0x74BBF0: `lea ecx,[ebx+0x48]; call 0x6D5980` at 0x74BCAC */
#define Z_RA_ARRIVAL 0x74C289u    /* EventCollisionArrivalCharaState 0x74BFA0: `lea ecx,[ebx+0x48]; call 0x6D5980` at 0x74C284 */

typedef unsigned (TC *zhit_fn)(void *self, int idx);
static zhit_fn orig_zhit;
static volatile int f_zone = 1;
static z1_t ZT;
static CRITICAL_SECTION zcs;
static volatile LONG zcs_ready;
static volatile uint32_t z_own, z_pup;   /* the own human's unit / the partner's puppet unit as the last logic pass saw them (0 = none: the rule rests) */
static volatile LONG z_calls, z_scoped;
static unsigned z_logged;
static volatile uint32_t zw_mask;         /* the watched records (bit = record index < 32; 0 = no watch) */
static volatile uint32_t zw_until;
static uint32_t zw_last[32];
static int zw_last_r[32];
static char zw_part[48];
static unsigned zw_lines;

static void zone_lock(void)
{
    if (!zcs_ready) {   /* first use: world_init runs before any hook, but the lock is made here so the include stays self-contained */
        static volatile LONG once;
        if (InterlockedCompareExchange(&once, 1, 0) == 0) InitializeCriticalSection(&zcs), zcs_ready = 1;
        while (!zcs_ready) Sleep(0);
    }
    EnterCriticalSection(&zcs);
}
static void zone_unlock(void) { LeaveCriticalSection(&zcs); }

/* the ECD resource's path (resource + 8) for the log; the game's own string, read through the safe reader */
static void zone_path(uint32_t res, char *out, unsigned size)
{
    out[0] = 0;
    if (!res || size < 2) return;
    unsigned n = size - 1 < 64 ? size - 1 : 64;
    if (!r1_safe_read((const void *)(uintptr_t)(res + 8), out, n)) {
        snprintf(out, size, "?");
        return;
    }
    out[n] = 0;
}

/* the puppet as the stock unit test would take it: still the partner's unit in the players list, in play, of a class the record asks for */
static int zone_pup_counts(void *mp, uint32_t pup)
{
    uint32_t u = 0, mask = (uint32_t)(uintptr_t)mp;
    if (!pup || !wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &u) || u != pup) return 0;
    return z1_unit_counts(rd32(pup + 0x0C), wh_unit_class(&MEM, pup), mask);
}

/* LAB probe (`world zone watch`): one line per test of the watched record (the first, a change of the stock result, else once a second) */
static void zone_watch(uint32_t res, uint32_t rec, int idx, unsigned r, volatile uint32_t *sp)
{
    uint32_t now = GetTickCount();
    if ((int32_t)(zw_until - now) <= 0) {
        if (zw_mask) zw_mask = 0, wlog("zone: watch over (%u lines)", zw_lines);
        return;
    }
    char path[72];
    zone_path(res, path, sizeof path);
    if (zw_part[0]) {
        size_t lp = strlen(zw_part), ln = strlen(path);
        if (zw_part[lp - 1] == '$') {
            if (ln < lp - 1 || strncmp(path + ln - (lp - 1), zw_part, lp - 1)) return;
        } else if (!strstr(path, zw_part)) return;
    }
    int rr = (r & 0xFF) != 0;
    uint32_t ph = z1_key(path, 64);   /* per record and file: the first test, a change of the stock result, else once a second */
    if (zw_last_r[idx] == (int)((ph & 0x7FFFFFFEu) | (uint32_t)rr) && (uint32_t)(now - zw_last[idx]) < 1000u) return;
    zw_last[idx] = now, zw_last_r[idx] = (int)((ph & 0x7FFFFFFEu) | (uint32_t)rr);
    if (++zw_lines > 2000) return;
    uint32_t own = z_own, me = own ? rd32(own + 0xE88) : 0, s[4] = {0, 0, 0, 0};
    if (sp) r1_safe_read((const void *)sp, s, sizeof s);
    int ub[6] = {-1, -1, -1, -1, -1, -1}, rb[6];
    for (int i = 0; i < 3 && me; i++) ub[2 * i] = (int)rd32(me + 0x298 + 4u * i), ub[2 * i + 1] = (int)rd32(me + 0x2A4 + 4u * i);
    for (int i = 0; i < 3; i++) rb[2 * i] = (int)rd32(rec + 0x90 + 4u * i), rb[2 * i + 1] = (int)rd32(rec + 0x9C + 4u * i);
    float qy = own ? rdf(own + 0x54) : 0, qw = own ? rdf(own + 0x5C) : 1, yaw = 2.0f * atan2f(qy, qw) * 57.29578f;
    float cx = rdf(rec + 0x10) + rdf(rec + 0x40), cy = rdf(rec + 0x14) + rdf(rec + 0x44), cz = rdf(rec + 0x18) + rdf(rec + 0x48);
    float ox = own ? rdf(own + 0x40) : 0, oy = own ? rdf(own + 0x44) : 0, oz = own ? rdf(own + 0x48) : 0;
    /* two lines: wlog keeps 300 characters (l9a: one line lost the record's gates) */
    wlog("zone: WATCH %s[%d] stock %d hitters %08x %08x %08x %08x (own %08x, puppet %08x) | own (%.1f, %.1f, %.1f) %.1f u from the centre, yaw %.1f deg, blocks %d/%d %d/%d %d/%d",
         path, idx, rr, (unsigned)s[0], (unsigned)s[1], (unsigned)s[2], (unsigned)s[3], (unsigned)own, (unsigned)z_pup, (double)ox, (double)oy, (double)oz,
         (double)sqrtf((ox - cx) * (ox - cx) + (oz - cz) * (oz - cz)), (double)yaw, ub[0], ub[1], ub[2], ub[3], ub[4], ub[5]);
    wlog("zone: WATCH %s[%d] record: centre (%.1f, %.1f, %.1f) r %.1f, kind %d type %u mask %08x all %u, blocks %d/%d %d/%d %d/%d (gate %s), facing gate %u ref %u %.1f..%.1f deg, cc %u",
         path, idx, (double)cx, (double)cy, (double)cz, (double)rdf(rec + 0x60), (int)rd32(rec + 4), rd32(rec + 0xB8), rd32(rec + 0xBC), rd32(rec + 0xC8), rb[0], rb[1], rb[2], rb[3], rb[4], rb[5],
         (rd32(rec + 0xB8) & ~2u) == 1 ? "on" : "off", rd8(rec + 0xA8), rd32(rec + 0xAC), (double)rdf(rec + 0xB0), (double)rdf(rec + 0xB4), rd8(rec + 0xCC));
}

static unsigned zone_after(uint32_t ec, int idx, unsigned r)
{
    const volatile uint8_t *ecp = (const volatile uint8_t *)(uintptr_t)ec;
    uint32_t res = *(const volatile uint32_t *)(ecp + 8);
    if (!ecp[0x0C] || !res) return r;   /* the object is off (the stock returned 0 without a test) */
    const volatile uint8_t *rp = (const volatile uint8_t *)(uintptr_t)res;
    if ((uint32_t)idx >= *(const volatile uint32_t *)(rp + 0x74)) return r;
    uint32_t recs = *(const volatile uint32_t *)(rp + 0x80), en = *(const volatile uint32_t *)(ecp + 0x20), hl = *(const volatile uint32_t *)(ecp + 0x18);
    if (!recs || !en || !hl || (uint32_t)idx >= *(const volatile uint32_t *)(ecp + 0x1C) || (uint32_t)idx >= *(const volatile uint32_t *)(ecp + 0x24)) return r;
    uint32_t rec = ((const volatile uint32_t *)(uintptr_t)recs)[idx];
    if (!rec || !((const volatile uint8_t *)(uintptr_t)en)[idx]) return r;   /* no record / the record is disabled here: the stock did not test it */
    const volatile uint8_t *rcp = (const volatile uint8_t *)(uintptr_t)rec;
    uint32_t type = *(const volatile uint32_t *)(rcp + 0xB8), mask = *(const volatile uint32_t *)(rcp + 0xBC), all = *(const volatile uint32_t *)(rcp + 0xC8);
    volatile uint32_t *sp = (volatile uint32_t *)(uintptr_t)((const volatile uint32_t *)(uintptr_t)hl)[2 * idx];
    if (idx < 32 && (zw_mask >> idx & 1u)) zone_watch(res, rec, idx, r, sp);   /* LAB probe: before the rule, what the stock saw */
    if (!z1_rec_ok(type, all)) return r;
    if (!sp) return r;
    uint32_t s[4] = {sp[0], sp[1], sp[2], sp[3]}, own = z_own, pup = z_pup, now = GetTickCount();
    char path[72];
    uint32_t key;
    {
        char tmp[65];
        int k;
        for (k = 0; k < 64; k++) {
            tmp[k] = (char)rp[8 + k];
            if (!tmp[k]) break;
        }
        tmp[k < 64 ? k : 64] = 0;
        key = z1_key(tmp, 64);
    }
    int d = 0, changed = 0, pup_hit = 0;
    for (int j = 0; j < 4; j++) pup_hit |= pup && s[j] == pup;
    unsigned was = (r & 0xFF) != 0;
    zone_lock();
    int any = z1_rule(&ZT, key, (unsigned)idx, now, s, own, pup, (int)was, zone_pup_counts, (void *)(uintptr_t)mask, &d, &changed);
    zone_unlock();
    if (!d) return r;   /* no puppet in it, no report, nothing to log: stock */
    int count = 0;
    for (int j = 0; j < 4; j++) count |= pup && s[j] == pup;   /* the puppet is a hitter after the rule (counted by the report) */
    if (changed)
        for (int j = 0; j < 4; j++) sp[j] = s[j];
    if ((unsigned)any != was) {   /* the hit latch of the record ([this+0x10] + record), as the stock sets it on a hit */
        uint32_t lp = *(const volatile uint32_t *)(ecp + 0x10);
        if (lp && (uint32_t)idx < *(const volatile uint32_t *)(ecp + 0x14)) ((volatile uint8_t *)(uintptr_t)lp)[idx] = (uint8_t)any;
    }
    if ((d & (Z1_D_LOG_OWN | Z1_D_LOG_DROP | Z1_D_LOG_REP)) && z_logged < 600) {
        z_logged++;
        zone_path(res, path, sizeof path);
        if (d & Z1_D_LOG_OWN) wlog("zone: %s[%d] (key %08x): the own human (unit %08x, slot %d) hit it here: reported to the partner", path, idx, (unsigned)key, (unsigned)own, (int)g_own);
        if (d & Z1_D_LOG_DROP) wlog("zone: %s[%d] (key %08x): the partner's PUPPET (unit %08x) hit it here, his PC reports no hit: NOT counted (result %u -> %d)", path, idx, (unsigned)key, (unsigned)pup, was, any);
        if (d & Z1_D_LOG_REP)
            wlog("zone: %s[%d] (key %08x): the partner's PC reports his human in it: %s (puppet unit %08x %s, result %u -> %d)", path, idx, (unsigned)key, count ? "counted as his unit's hit here" : "NOT counted (his unit is not in play here / not of the record's class)",
                 (unsigned)pup, pup_hit ? "also stood in it here" : "was not in it here", was, any);
    }
    if (changed || (unsigned)any != was) return (unsigned)any;
    return r;
}

static unsigned TC hook_zhit(void *self, int idx)
{
    unsigned r = orig_zhit(self, idx);
    uint32_t ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
    InterlockedIncrement(&z_calls);
    if ((ra != Z_RA_HITCHECK && ra != Z_RA_ARRIVAL) || !g_active || !f_zone || idx < 0 || !z_pup) return r;
    InterlockedIncrement(&z_scoped);
    return zone_after((uint32_t)(uintptr_t)self, idx, r);
}

static void zone_reset(const char *why);
/* logic pass (model lock held, every frame while bound): a new session / level instance forgets the table; while PLAYING the unit pointers for the hook
 * (0 otherwise: a load frees the units, the hook then rests), then the queued reports go out */
static void zone_pass(int playing)
{
    static int env_done;
    if (!env_done) {
        char v[8];
        env_done = 1;
        if (r1_env("REV1_ZONE", v, sizeof v) && v[0] == '0') f_zone = 0, wlog("zone: REV1_ZONE=0: the zone rule starts OFF (lab)");
    }
    static cs_t *z_cs;
    static unsigned z_ep = 0xFFFFFFFFu;
    unsigned ep = w1_epoch(&N.w);
    if (bound_cs != z_cs || ep != z_ep) {
        char why[48];
        snprintf(why, sizeof why, "level instance %u", ep);
        zone_reset(why);
        z_cs = bound_cs, z_ep = ep;
    }
    uint32_t own = 0, pup = 0;
    if (playing && g_active && f_zone) {
        wh_player_by_slot(&MEM, (unsigned)g_own, &own);
        wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &pup);
    }
    z_own = own, z_pup = pup;
    if (!own || !pup) return;
    for (int k = 0; k < 24; k++) {
        uint32_t key;
        unsigned idx, st;
        zone_lock();
        int ok = z1_next_send(&ZT, &key, &idx, &st);
        zone_unlock();
        if (!ok) break;
        uint8_t b[8];
        unsigned n = z1_enc(b, sizeof b, key, idx, st);
        if (!n || !w1_send_ext(&N.w, CP_W1_ZONE, b, n)) wlog("zone: report key %08x[%u] %s NOT queued", (unsigned)key, idx, st ? "in" : "out");
    }
}

/* a new level instance / unbound: the old level's zones and reports are gone */
static void zone_reset(const char *why)
{
    zone_lock();
    unsigned n = ZT.n;
    z1_reset(&ZT);
    zone_unlock();
    z_own = z_pup = 0;
    if (n) wlog("zone: table reset (%s): %u records forgotten", why, n);
}

/* the model: a report of the partner's PC (ops.ext, logic thread) */
static void zone_rx(const uint8_t *b, unsigned len)
{
    uint32_t key;
    unsigned idx, st;
    if (!f_zone) return;
    zone_lock();
    if (!z1_dec(b, len, &key, &idx, &st)) {
        ZT.st.rx_bad++;
        zone_unlock();
        return;
    }
    z1_remote(&ZT, key, idx, st, GetTickCount());
    zone_unlock();
}

static void zone_cmd(const char *a2, const char *arg, char *out, int size)
{
    if (!strcmp(a2, "on") || !strcmp(a2, "off")) {
        f_zone = !strcmp(a2, "on");
        if (!f_zone) z_own = z_pup = 0;
        snprintf(out, (size_t)size, "world: zone rule %s (LAB when off: the partner's puppet counts in the story's zones by its own position again)", f_zone ? "ON" : "off");
        return;
    }
    if (!strcmp(a2, "watch")) {   /* LAB: world zone watch <record> [seconds] [path part] | off */
        char a3[16] = "", a4[16] = "", a5[48] = "";
        sscanf(arg, "%*s %*s %15s %15s %47s", a3, a4, a5);   /* zone watch <record> [seconds] [path part] */
        if (!strcmp(a3, "off") || !a3[0]) {
            zw_mask = 0;
            snprintf(out, (size_t)size, "world: zone watch off (%u lines)", zw_lines);
            return;
        }
        int sec = a4[0] ? atoi(a4) : 120;
        uint32_t m = 0;
        for (const char *c = a3; *c;) {
            int v = atoi(c);
            if (v >= 0 && v < 32) m |= 1u << v;
            while (*c && *c != ',') c++;
            if (*c == ',') c++;
        }
        snprintf(zw_part, sizeof zw_part, "%s", a5);
        memset(zw_last_r, 0xFF, sizeof zw_last_r), zw_lines = 0, zw_until = GetTickCount() + (uint32_t)(sec > 0 ? sec : 120) * 1000u;
        zw_mask = m;
        snprintf(out, (size_t)size, "world: LAB zone watch of records %s (mask %08x) for %d s (paths %s '%s')", a3, (unsigned)m, sec > 0 ? sec : 120, zw_part[0] ? "matching" : "any", zw_part);
        return;
    }
    if (!strcmp(a2, "reset")) {
        zone_reset("console");
        snprintf(out, (size_t)size, "world: zone table reset");
        return;
    }
    zone_lock();
    z1_stats s = ZT.st;
    unsigned n = ZT.n, live = 0, inside = 0;
    uint32_t now = GetTickCount();
    for (unsigned i = 0; i < n; i++) live += ZT.e[i].rem_on && (int32_t)(ZT.e[i].rem_until - now) > 0, inside += ZT.e[i].own_in;
    zone_unlock();
    snprintf(out, (size_t)size,
             "world zone: rule %s, hook %s; own %08x puppet %08x; calls %ld (FSM %ld); records %u (own human in %u, partner reports live %u); tests %u own hits %u, sent in %u out %u, received in %u "
             "out %u bad %u, puppet counted by report %u, puppet dropped %u, evicted %u",
             f_zone ? "ON" : "off", wh_state_name(H.state[WH_ZONEHIT]), (unsigned)z_own, (unsigned)z_pup, (long)z_calls, (long)z_scoped, n, inside, live, s.tests, s.own_hits, s.sent_in, s.sent_out,
             s.rx_in, s.rx_out, s.rx_bad, s.counted, s.dropped, s.evicted);
}
