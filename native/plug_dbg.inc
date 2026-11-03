/*
 * Live debugging tools of the plug-in (included by plug_cap.c; needs rd32, hx, A, the log).
 *
 *   P snap <name> <hexaddr> <hexlen>      remember memory
 *   P usnap <name> <unit|partner|main|hex> [depth-1 pointer targets, 0x200 bytes each]: the unit and everything its pointer fields lead to
 *   P diff <name>                         changed dwords since the snapshot (with float reading)
 *   P find <hexval|f:float> <hexaddr> <hexlen>   dwords equal to a value
 *   P ptrs <hexaddr> <hexlen>             pointer fields that lead to a game object (class name)
 *   P bp <hexaddr> [maxlog] [ecx=<hex>]   INT3 call logger (one byte patched, restored for the single step): ecx, edx, return address, 4 stack words
 *   P bp off | list | log
 *   P dr <i 0-3> <hexaddr> <r|w|rw> [len 1|2|4]   hardware data watch on the game thread; P dr dump [reset] = histogram of the instructions that touched it
 *   P dr off
 */

/* ---- snapshots ---- */
#define NSNAP 8
#define MAXREG 300
typedef struct {
    unsigned src, addr, len;
    unsigned char *data;
} reg_t;
typedef struct {
    char name[16];
    int n;
    reg_t r[MAXREG];
} snap_t;
static snap_t *snaps[NSNAP];

static snap_t *snap_get(const char *name, int create)
{
    for (int i = 0; i < NSNAP; i++)
        if (snaps[i] && !strcmp(snaps[i]->name, name))
            return snaps[i];
    if (!create)
        return NULL;
    for (int i = 0; i < NSNAP; i++)
        if (!snaps[i]) {
            snaps[i] = (snap_t *)calloc(1, sizeof(snap_t));
            snprintf(snaps[i]->name, sizeof snaps[i]->name, "%s", name);
            return snaps[i];
        }
    return NULL;
}

static void snap_free(snap_t *s)
{
    for (int i = 0; i < s->n; i++)
        free(s->r[i].data);
    s->n = 0;
}

/* readable prefix of [addr, addr+want) in 0x40-byte steps */
static unsigned readable_len(unsigned addr, unsigned want)
{
    unsigned got = 0;
    unsigned char tmp[0x40];
    while (got < want) {
        unsigned n = want - got < sizeof tmp ? want - got : (unsigned)sizeof tmp;
        if (!A->safe_read((void *)(addr + got), tmp, n))
            break;
        got += n;
    }
    return got;
}

static int snap_add(snap_t *s, unsigned src, unsigned addr, unsigned len)
{
    if (s->n >= MAXREG)
        return 0;
    unsigned have = readable_len(addr, len);
    if (!have)
        return 0;
    unsigned char *d = (unsigned char *)malloc(have);
    if (!d)
        return 0;
    A->safe_read((void *)addr, d, have);
    s->r[s->n].src = src, s->r[s->n].addr = addr, s->r[s->n].len = have, s->r[s->n].data = d;
    s->n++;
    return 1;
}

static int looks_float(unsigned v)
{
    unsigned e = (v >> 23) & 0xFF;
    return v && e >= 100 && e <= 160;
}

static int heap_ptr(unsigned p)
{
    return p > 0x01230000u && p < 0x7FFE0000u && !(p & 3);
}

static void snap_diff(snap_t *s, char *out, int size)
{
    int o = 0, lines = 0;
    for (int r = 0; r < s->n && o + 100 < size; r++) {
        reg_t *g = &s->r[r];
        unsigned char *cur = (unsigned char *)malloc(g->len);
        if (!cur)
            continue;
        if (!A->safe_read((void *)g->addr, cur, g->len)) {
            free(cur);
            o += snprintf(out + o, size - o, "region %08x unreadable now\n", g->addr);
            continue;
        }
        for (unsigned i = 0; i + 4 <= g->len && o + 100 < size && lines < 150; i += 4) {
            unsigned a = *(unsigned *)(g->data + i), b = *(unsigned *)(cur + i);
            if (a == b)
                continue;
            float fa, fb;
            memcpy(&fa, &a, 4), memcpy(&fb, &b, 4);
            if (g->src == 0xFFFFFFFFu || r == 0)
                o += snprintf(out + o, size - o, "[%08x+%03x] %08x -> %08x", g->addr, i, a, b);
            else
                o += snprintf(out + o, size - o, "[unit+%03x ->%08x +%03x] %08x -> %08x", g->src, g->addr, i, a, b);
            if (looks_float(a) || looks_float(b))
                o += snprintf(out + o, size - o, "  (%.3f -> %.3f)", fa, fb);
            else if (a < 100000 && b < 100000)
                o += snprintf(out + o, size - o, "  (%u -> %u)", a, b);
            o += snprintf(out + o, size - o, "\n");
            lines++;
        }
        free(cur);
    }
    if (!lines)
        o += snprintf(out + o, size - o, "no change in %d regions", s->n);
    else if (lines >= 150)
        o += snprintf(out + o, size - o, "... (150 lines shown)");
}

static void dbg_snap(const char *arg, char *out, int size, int unit_mode)
{
    char name[16] = "", a1[40] = "", a2[40] = "";
    if (sscanf(arg, "%15s %39s %39s", name, a1, a2) < 2) {
        snprintf(out, size, "usage: snap <name> <hexaddr> <hexlen> | usnap <name> <unit|partner|main|hex>");
        return;
    }
    snap_t *s = snap_get(name, 1);
    if (!s) {
        snprintf(out, size, "snapshot table full (8)");
        return;
    }
    snap_free(s);
    if (!unit_mode) {
        unsigned len = hx(a2);
        if (!len)
            len = 0x100;
        snprintf(out, size, snap_add(s, 0xFFFFFFFFu, hx(a1), len) ? "snapshot %s: %u bytes at %08x" : "snapshot %s: %u bytes unreadable at %08x", name, len, hx(a1));
        return;
    }
    unsigned unit = find_unit(a1);
    if (!unit) {
        snprintf(out, size, "no such unit");
        return;
    }
    if (!snap_add(s, 0, unit, 0x1760)) {
        snprintf(out, size, "unit unreadable");
        return;
    }
    int np = 0;
    unsigned char *u = s->r[0].data;
    for (unsigned i = 0; i + 4 <= s->r[0].len && np < MAXREG - 2; i += 4) {
        unsigned p = *(unsigned *)(u + i);
        if (!heap_ptr(p) || (p >= unit && p < unit + 0x1760))
            continue;
        if (snap_add(s, i, p, 0x200))
            np++;
    }
    snprintf(out, size, "unit snapshot %s: unit %08x + %d pointer targets", name, unit, np);
}

static void dbg_find(const char *arg, char *out, int size)
{
    char v[40] = "", a[40] = "", l[40] = "";
    if (sscanf(arg, "%39s %39s %39s", v, a, l) < 3) {
        snprintf(out, size, "usage: find <hexval|f:float> <hexaddr> <hexlen>");
        return;
    }
    unsigned val;
    if (v[0] == 'f' && v[1] == ':') {
        float f = (float)atof(v + 2);
        memcpy(&val, &f, 4);
    } else
        val = hx(v);
    unsigned base = hx(a), len = hx(l);
    int o = 0, n = 0;
    for (unsigned i = 0; i + 4 <= len && o + 40 < size && n < 100; i += 4)
        if (rd32(base + i) == val) {
            o += snprintf(out + o, size - o, "+%03x ", i);
            n++;
        }
    if (!n)
        snprintf(out, size, "not found");
}

static void dbg_ptrs(const char *arg, char *out, int size)
{
    char a[40] = "", l[40] = "";
    sscanf(arg, "%39s %39s", a, l);
    unsigned base = hx(a), len = hx(l);
    if (!len)
        len = 0x100;
    int o = 0;
    for (unsigned i = 0; i + 4 <= len && o + 120 < size; i += 4) {
        unsigned p = rd32(base + i);
        char cn[96];
        if (p > 0x10000 && !(p & 3) && A->object_name(p, cn, sizeof cn))
            o += snprintf(out + o, size - o, "+%03x %08x %s\n", i, p, cn);
    }
    if (!o)
        snprintf(out, size, "no object pointers in %08x+%x", base, len);
}

/* ---- INT3 call logger and hardware data watch (one vectored handler) ---- */
#define NBP 16
static struct {
    unsigned addr;
    unsigned char orig;
    int on, filter;
    unsigned ecx;
    volatile unsigned hits, maxlog;
} bps[NBP];
static volatile unsigned bp_pend_addr;
static volatile DWORD bp_pend_tid;
static char bplog[48][150];
static volatile int bplog_n;
static PVOID veh_h;

#define NHIST 256
static struct {
    unsigned eip, ret, ret2, regs, count, kind;
} hist[NHIST];
static volatile int nhist;
static struct {
    unsigned addr;
    int rw, len, on;
} dwt[4];
static volatile unsigned dr_events;

static void bp_put(unsigned addr, unsigned char b)
{
    DWORD old;
    if (VirtualProtect((void *)addr, 1, PAGE_EXECUTE_READWRITE, &old)) {
        *(volatile unsigned char *)addr = b;
        FlushInstructionCache(GetCurrentProcess(), (void *)addr, 1);
    }
}

static LONG CALLBACK dbg_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT *c = ep->ContextRecord;
    if (code == EXCEPTION_BREAKPOINT) {
        unsigned a = (unsigned)ep->ExceptionRecord->ExceptionAddress;
        for (int i = 0; i < NBP; i++) {
            if (!bps[i].on || bps[i].addr != a)
                continue;
            if (!bps[i].filter || c->Ecx == bps[i].ecx) {
                unsigned h = ++bps[i].hits;
                if (h <= bps[i].maxlog) {
                    unsigned st[5] = {0};
                    A->safe_read((void *)c->Esp, st, sizeof st);
                    int k = bplog_n++ % 48;
                    snprintf(bplog[k], sizeof bplog[k], "bp %08x #%u tid %lu ecx=%08x edx=%08x eax=%08x ret=%08x args %08x %08x %08x %08x", a, h, GetCurrentThreadId(), (unsigned)c->Ecx, (unsigned)c->Edx, (unsigned)c->Eax, st[0], st[1], st[2],
                             st[3], st[4]);
                }
            }
            bp_put(a, bps[i].orig);
            c->Eip = a;
            c->EFlags |= 0x100;
            bp_pend_addr = a;
            bp_pend_tid = GetCurrentThreadId();
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        if (bp_pend_addr && bp_pend_tid == GetCurrentThreadId()) {
            unsigned a = bp_pend_addr;
            bp_pend_addr = 0;
            for (int i = 0; i < NBP; i++)
                if (bps[i].on && bps[i].addr == a)
                    bp_put(a, 0xCC);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (c->Dr6 & 0xF) {
            unsigned eip = (unsigned)ep->ExceptionRecord->ExceptionAddress, ret = 0, ret2 = 0;
            A->safe_read((void *)c->Esp, &ret, 4);
            if (c->Ebp)
                A->safe_read((void *)(c->Ebp + 4), &ret2, 4);
            int kind = (int)(c->Dr6 & 0xF), found = -1;
            for (int i = 0; i < nhist; i++)
                if (hist[i].eip == eip && hist[i].kind == (unsigned)kind) {
                    found = i;
                    break;
                }
            if (found < 0 && nhist < NHIST) {
                found = nhist;
                hist[found].eip = eip, hist[found].ret = ret, hist[found].ret2 = ret2, hist[found].regs = c->Eax, hist[found].kind = (unsigned)kind;
                nhist++;
            }
            if (found >= 0)
                hist[found].count++;
            dr_events++;
            c->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void veh_ensure(void)
{
    if (!veh_h)
        veh_h = AddVectoredExceptionHandler(1, dbg_veh);
}

struct drreq {
    DWORD tid;
    unsigned addr[4];
    unsigned dr7;
    int ok;
};

static DWORD WINAPI dr_thread(LPVOID p)
{
    struct drreq *r = (struct drreq *)p;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, r->tid);
    if (!h)
        return 1;
    if (SuspendThread(h) != (DWORD)-1) {
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &c)) {
            c.Dr0 = r->addr[0], c.Dr1 = r->addr[1], c.Dr2 = r->addr[2], c.Dr3 = r->addr[3];
            c.Dr6 = 0;
            c.Dr7 = r->dr7;
            r->ok = SetThreadContext(h, &c) ? 1 : 0;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return 0;
}

static int dr_apply(void)
{
    struct drreq r;
    memset(&r, 0, sizeof r);
    r.tid = GetCurrentThreadId();
    for (int i = 0; i < 4; i++)
        if (dwt[i].on) {
            r.addr[i] = dwt[i].addr;
            unsigned rw = dwt[i].rw == 1 ? 1u : 3u;   /* 1 = write, 3 = read/write (x86 has no read-only) */
            unsigned len = dwt[i].len == 1 ? 0u : dwt[i].len == 2 ? 1u : 3u;
            r.dr7 |= (1u << (i * 2)) | (rw << (16 + i * 4)) | (len << (18 + i * 4));
        }
    HANDLE t = CreateThread(NULL, 0, dr_thread, &r, 0, NULL);
    if (!t)
        return 0;
    WaitForSingleObject(t, 3000);
    CloseHandle(t);
    return r.ok;
}

static int cmp_hist(const void *a, const void *b)
{
    const unsigned *x = (const unsigned *)a, *y = (const unsigned *)b;   /* count is the 5th member */
    return (int)y[4] - (int)x[4];
}

static void dbg_dr(const char *arg, char *out, int size)
{
    char s1[16] = "", s2[40] = "", s3[8] = "";
    int len = 4, idx = 0;
    sscanf(arg, "%15s %39s %7s %d", s1, s2, s3, &len);
    veh_ensure();
    if (!strcmp(s1, "off")) {
        memset(dwt, 0, sizeof dwt);
        int ok = dr_apply();
        snprintf(out, size, "hardware watches cleared (%s)", ok ? "ok" : "failed");
        return;
    }
    if (!strcmp(s1, "dump")) {
        int reset = !strcmp(s2, "reset");
        int n = nhist;
        qsort(hist, (size_t)n, sizeof hist[0], cmp_hist);
        int o = snprintf(out, size, "%u events, %d distinct instructions (eip = the instruction AFTER the access; ret = [esp], ret2 = [ebp+4])\n", dr_events, n);
        for (int i = 0; i < n && o + 120 < size; i++)
            o += snprintf(out + o, size - o, "%08x x%-6u ret=%08x ret2=%08x eax=%08x dr6=%x\n", hist[i].eip, hist[i].count, hist[i].ret, hist[i].ret2, hist[i].regs, hist[i].kind);
        if (reset) {
            nhist = 0;
            dr_events = 0;
            memset(hist, 0, sizeof hist);
        }
        return;
    }
    idx = atoi(s1);
    if (idx < 0 || idx > 3 || !s2[0]) {
        snprintf(out, size, "usage: dr <0-3> <hexaddr> <r|w|rw> [len 1|2|4] | dr dump [reset] | dr off");
        return;
    }
    dwt[idx].addr = hx(s2) & ~(unsigned)(len == 2 ? 1 : len == 1 ? 0 : 3);
    dwt[idx].rw = !strcmp(s3, "w") ? 1 : 3;
    dwt[idx].len = len == 1 || len == 2 ? len : 4;
    dwt[idx].on = 1;
    nhist = 0;
    dr_events = 0;
    memset(hist, 0, sizeof hist);
    int ok = dr_apply();
    snprintf(out, size, "watch %d on %08x (%s, %d bytes) on thread %lu: %s", idx, dwt[idx].addr, dwt[idx].rw == 1 ? "write" : "read/write", dwt[idx].len, GetCurrentThreadId(), ok ? "armed" : "FAILED");
}

static void dbg_bp(const char *arg, char *out, int size)
{
    char s1[40] = "", s2[40] = "", s3[40] = "";
    sscanf(arg, "%39s %39s %39s", s1, s2, s3);
    veh_ensure();
    if (!strcmp(s1, "off")) {
        for (int i = 0; i < NBP; i++)
            if (bps[i].on) {
                bp_put(bps[i].addr, bps[i].orig);
                bps[i].on = 0;
            }
        snprintf(out, size, "all breakpoints removed");
        return;
    }
    if (!strcmp(s1, "list")) {
        int o = 0;
        for (int i = 0; i < NBP; i++)
            if (bps[i].on)
                o += snprintf(out + o, size - o, "%08x hits %u (log %u)%s\n", bps[i].addr, bps[i].hits, bps[i].maxlog, bps[i].filter ? " filtered" : "");
        if (!o)
            snprintf(out, size, "no breakpoints");
        return;
    }
    if (!strcmp(s1, "log")) {
        int o = 0, n = bplog_n, from = n > 48 ? n - 48 : 0;
        for (int i = from; i < n && o + 160 < size; i++)
            o += snprintf(out + o, size - o, "%s\n", bplog[i % 48]);
        if (!o)
            snprintf(out, size, "(empty)");
        return;
    }
    unsigned addr = hx(s1);
    if (!addr) {
        snprintf(out, size, "usage: bp <hexaddr> [maxlog] [ecx=hex] | bp off|list|log");
        return;
    }
    int slot = -1;
    for (int i = 0; i < NBP; i++) {
        if (bps[i].on && bps[i].addr == addr) {
            slot = i;
            break;
        }
        if (!bps[i].on && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        snprintf(out, size, "no free breakpoint");
        return;
    }
    if (!bps[slot].on) {
        unsigned char b = (unsigned char)rd8(addr);
        if (b == 0xCC || !A->safe_read((void *)addr, &b, 1)) {
            snprintf(out, size, "cannot patch %08x", addr);
            return;
        }
        bps[slot].orig = b;
        bps[slot].addr = addr;
        bps[slot].hits = 0;
        bps[slot].on = 1;
        bp_put(addr, 0xCC);
    }
    bps[slot].maxlog = s2[0] ? (unsigned)atoi(s2) : 20;
    bps[slot].filter = 0;
    if (!strncmp(s3, "ecx=", 4) || !strncmp(s2, "ecx=", 4)) {
        bps[slot].filter = 1;
        bps[slot].ecx = hx(strchr(s3[0] == 'e' ? s3 : s2, '=') + 1);
        if (s2[0] == 'e')
            bps[slot].maxlog = 20;
    }
    snprintf(out, size, "breakpoint %d at %08x (orig byte %02x), log %u hits%s", slot, addr, bps[slot].orig, bps[slot].maxlog, bps[slot].filter ? ", ecx filter" : "");
}

static void dbg_cleanup(void)
{
    for (int i = 0; i < NBP; i++)
        if (bps[i].on) {
            bp_put(bps[i].addr, bps[i].orig);
            bps[i].on = 0;
        }
    if (dwt[0].on || dwt[1].on || dwt[2].on || dwt[3].on) {
        memset(dwt, 0, sizeof dwt);
        dr_apply();
    }
    if (veh_h) {
        RemoveVectoredExceptionHandler(veh_h);
        veh_h = NULL;
    }
    for (int i = 0; i < NSNAP; i++)
        if (snaps[i]) {
            snap_free(snaps[i]);
            free(snaps[i]);
            snaps[i] = NULL;
        }
}

static int dbg_cmd(const char *name, const char *arg, char *out, int size)
{
    if (!strcmp(name, "snap"))
        dbg_snap(arg, out, size, 0);
    else if (!strcmp(name, "usnap"))
        dbg_snap(arg, out, size, 1);
    else if (!strcmp(name, "diff")) {
        char nm[16] = "";
        sscanf(arg, "%15s", nm);
        snap_t *s = snap_get(nm, 0);
        if (!s)
            snprintf(out, size, "no snapshot '%s'", nm);
        else
            snap_diff(s, out, size);
    } else if (!strcmp(name, "find"))
        dbg_find(arg, out, size);
    else if (!strcmp(name, "ptrs"))
        dbg_ptrs(arg, out, size);
    else if (!strcmp(name, "bp"))
        dbg_bp(arg, out, size);
    else if (!strcmp(name, "dr"))
        dbg_dr(arg, out, size);
    else
        return 0;
    return 1;
}
