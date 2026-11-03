/*
 * Frame-granular state monitor of the plug-in (included by plug_cap.c after unit_by_slot): logs every CHANGE of watched values with the frame number, so a button
 * press and the state it causes can be matched without console round trips (a console poll takes 0.5-1.5 s, the game runs 60-80 frames per second).
 *
 *   P mon preset            watch the standard set for both characters (unit slot field 0 and 1): upper state class, character state, health, aim/run flags, current weapon slot,
 *                           weapon class, clip, reserve ammo, kind, control type, position (every 500 ms when moved) and the held buttons of both game pads
 *   P mon add <label> <slot|abs> <u|f|b|c> <off>[,<off>...]
 *                           slot = unit slot field 0/1 (offsets are applied to the unit, all but the last are pointers to follow), abs = the first offset is an absolute address;
 *                           u dword, f float, b byte, c class name of the object the final dword points to
 *   P mon on|off|clear|list
 *   P mon dump [n]          the last n (default 40) log lines
 * Lines go to the DLL log as "mon f<frame> t<ms> <label> <old> -> <new>" and to a ring buffer.
 */
#define MON_MAX 48
#define MON_RING 300
typedef struct {
    char label[28];
    int slot;   /* unit slot field, -1 absolute */
    char kind;  /* u f b c, special: W class of the current weapon, C clip of the current weapon, P position */
    int n;
    unsigned off[5];
    char old[64];
    int have;
} mon_t;
static mon_t mons[MON_MAX];
static int mon_n, mon_on;
static char mon_ring[MON_RING][140];
static unsigned mon_ring_n;
static unsigned mon_t0;
static float mon_px[2], mon_pz[2];
static unsigned mon_ptick[2];

static void mon_line(const char *fmt, ...)
{
    char buf[140];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    snprintf(mon_ring[mon_ring_n % MON_RING], sizeof mon_ring[0], "%s", buf);
    mon_ring_n++;
    A->log("mon %s", buf);
}

static int mon_value(const mon_t *m, char *val, int vsize)
{
    unsigned base = 0, a;
    if (m->slot >= 0) {
        base = unit_by_slot((unsigned)m->slot);
        if (!base) {
            snprintf(val, vsize, "(no unit)");
            return 0;
        }
    }
    if (m->kind == 'W' || m->kind == 'C') {
        unsigned pack = rd32(base + 0xFE8), idx = pack ? rd32(pack + 0xD4) : 0xF, w = pack && idx < 16 ? rd32(pack + 4 + 4 * idx) : 0;
        if (m->kind == 'C') {
            snprintf(val, vsize, w ? "%d" : "-", (int)rd32(w + 0xF7C));
        } else {
            char cn[64] = "-";
            if (w && !A->object_name(w, cn, sizeof cn))
                strcpy(cn, "?");
            snprintf(val, vsize, "%s", cn);
        }
        return 1;
    }
    a = base;
    for (int i = 0; i < m->n; i++) {
        if (i < m->n - 1) {   /* a pointer to follow (absolute: the first offset is the address of a pointer cell) */
            a = rd32(a + m->off[i]);
            if (!a) {
                snprintf(val, vsize, "(null)");
                return 0;
            }
        } else {
            a += m->off[i];
        }
    }
    switch (m->kind) {
    case 'x': snprintf(val, vsize, "%08x", rd32(a)); break;
    case 'f': snprintf(val, vsize, "%.1f", rdf(a)); break;
    case 'b': snprintf(val, vsize, "%u", rd8(a)); break;
    case 'c': {
        char cn[64] = "-";
        unsigned p = rd32(a);
        if (p && !A->object_name(p, cn, sizeof cn))
            strcpy(cn, "?");
        snprintf(val, vsize, "%s", p ? cn : "null");
        break;
    }
    default: snprintf(val, vsize, "%d", (int)rd32(a)); break;
    }
    return 1;
}

static void mon_add(const char *label, int slot, char kind, int n, const unsigned *off)
{
    if (mon_n >= MON_MAX)
        return;
    mon_t *m = &mons[mon_n++];
    memset(m, 0, sizeof *m);
    snprintf(m->label, sizeof m->label, "%s", label);
    m->slot = slot, m->kind = kind, m->n = n > 5 ? 5 : n;
    for (int i = 0; i < m->n; i++)
        m->off[i] = off[i];
}

static void mon_preset(void)
{
    mon_n = 0;
    static const struct {
        const char *name;
        char kind;
        int n;
        unsigned off[3];
    } pre[] = {{"upper", 'c', 1, {0xF80}}, {"state", 'u', 1, {0xF08}}, {"hp", 'f', 1, {0xE38}}, {"aim", 'b', 1, {0x1151}}, {"run", 'b', 1, {0x1150}}, {"kind", 'u', 1, {0xE30}},
               {"ctl", 'u', 3, {0xE80, 0x38, 0x34}}, {"wslot", 'u', 2, {0xFE8, 0xD4}}, {"wpn", 'W', 0, {0}}, {"clip", 'C', 0, {0}}, {"reserve", 'u', 2, {0xFE8, 0x40}}};
    for (int s = 0; s < 2; s++)
        for (unsigned i = 0; i < sizeof pre / sizeof pre[0]; i++) {
            char lb[28];
            snprintf(lb, sizeof lb, "u%d.%s", s, pre[i].name);
            mon_add(lb, s, pre[i].kind, pre[i].n, pre[i].off);
        }
    unsigned g0 = 0x11E0388;   /* sGamePad pointer cell: Pad[k] held = [[g0]+0x3C+0x2CC*k+0x15C] (absolute: follow the cell, add the offset) */
    for (int k = 0; k < 2; k++) {
        char lb[28];
        unsigned off[2] = {g0, 0x3C + 0x2CC * (unsigned)k + 0x15C};
        snprintf(lb, sizeof lb, "pad%d.held", k);
        mon_add(lb, -1, 'x', 2, off);
    }
}

static void mon_frame(void)
{
    if (!mon_on)
        return;
    unsigned fr = A->frames();
    for (int i = 0; i < mon_n; i++) {
        mon_t *m = &mons[i];
        char v[64];
        if (!mon_value(m, v, sizeof v) && !m->have)
            continue;
        if (!m->have) {
            strcpy(m->old, v), m->have = 1;
            mon_line("f%u t%u %s = %s", fr, GetTickCount() - mon_t0, m->label, v);
        } else if (strcmp(m->old, v)) {
            mon_line("f%u t%u %s %s -> %s", fr, GetTickCount() - mon_t0, m->label, m->old, v);
            strcpy(m->old, v);
        }
    }
    for (int s = 0; s < 2; s++) {   /* position, at most every 500 ms and only when moved */
        unsigned u = unit_by_slot((unsigned)s);
        if (!u || GetTickCount() - mon_ptick[s] < 500)
            continue;
        float x = rdf(u + 0x40), z = rdf(u + 0x48);
        if (fabsf(x - mon_px[s]) + fabsf(z - mon_pz[s]) >= 5.0f) {
            mon_line("f%u t%u u%d.pos (%.0f,%.0f)", fr, GetTickCount() - mon_t0, s, x, z);
            mon_px[s] = x, mon_pz[s] = z;
        }
        mon_ptick[s] = GetTickCount();
    }
}

static int mon_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "", a1[28] = "", a2[8] = "", a3[4] = "", a4[80] = "";
    sscanf(arg, "%15s %27s %7s %3s %79s", sub, a1, a2, a3, a4);
    if (!strcmp(sub, "preset")) {
        mon_preset();
        mon_on = 1, mon_t0 = GetTickCount();
        snprintf(out, size, "monitor on: %d values (both characters, both pads)", mon_n);
    } else if (!strcmp(sub, "add")) {
        unsigned off[5];
        int n = 0;
        for (char *p = a4; *p && n < 5;) {
            off[n++] = (unsigned)strtoul(p, &p, 16);
            if (*p == ',')
                p++;
            else
                break;
        }
        mon_add(a1, !strcmp(a2, "abs") ? -1 : atoi(a2), a3[0], n, off);
        mon_on = 1;
        if (!mon_t0)
            mon_t0 = GetTickCount();
        snprintf(out, size, "watching %s (%d values)", a1, mon_n);
    } else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        mon_on = !strcmp(sub, "on");
        if (mon_on && !mon_t0)
            mon_t0 = GetTickCount();
        snprintf(out, size, "monitor %s", mon_on ? "on" : "off");
    } else if (!strcmp(sub, "clear")) {
        mon_n = 0, mon_ring_n = 0;
        snprintf(out, size, "cleared");
    } else if (!strcmp(sub, "list")) {
        int o = snprintf(out, size, "%d values, monitor %s\n", mon_n, mon_on ? "on" : "off");
        for (int i = 0; i < mon_n && o + 80 < size; i++)
            o += snprintf(out + o, size - o, "%s = %s\n", mons[i].label, mons[i].have ? mons[i].old : "?");
    } else if (!strcmp(sub, "dump")) {
        unsigned n = (unsigned)atoi(a1);
        if (!n)
            n = 40;
        if (n > MON_RING)
            n = MON_RING;
        if (n > mon_ring_n)
            n = mon_ring_n;
        int o = 0;
        for (unsigned i = mon_ring_n - n; i < mon_ring_n && o + 150 < size; i++)
            o += snprintf(out + o, size - o, "%s\n", mon_ring[i % MON_RING]);
        if (!n)
            snprintf(out, size, "(no lines)");
    } else {
        snprintf(out, size, "usage: P mon preset | add <label> <slot|abs> <u|f|b|c> <off>[,<off>..] | on | off | clear | list | dump [n]");
    }
    return 1;
}
