/*
 * Class-name walker for MT Framework objects (read only; nothing here calls game code).
 *
 *   object -> vtable (first dword) -> vtable[4] = the getter of the class's type info (DTI). The getter is a stub
 *   `mov eax, <DTI>; ret` (B8 imm32 C3) that is decoded from its bytes, so no game function is ever called.
 *   DTI: +4 class name (char *), +8 next sibling, +12 first child, +16 parent, +24 size field (low 23 bits = size in BYTES in Rev1, verified in the DTI constructor 0x8E1330; Rev2 stores dwords).
 *
 *   classname <addr>            "name (size 0x..) : parent : grandparent ..." for the object at addr
 *   dtiname <addr>              the same for a DTI address
 *   objscan <file> [filter]     every readable private memory block is scanned for dwords that point at a vtable of a known
 *                               class; per class counts to <file>, and every object address when a name filter is given
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"

static int sane_name(const char *s)
{
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return 0;
    for (int i = 1; i < 80; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0)
            return i >= 2;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '<' || c == '>' || c == ',' || c == ' ' || c == '*'))
            return 0;
    }
    return 0;
}

/* name of the DTI at address d, into out; 1 = a sane DTI */
static int dti_name(unsigned d, char *out, unsigned size_out, unsigned *parent, unsigned *size_field)
{
    unsigned w[8];
    char n[84];
    if (d < 0x10000 || (d & 3) || !r1_safe_read((void *)d, w, sizeof w))
        return 0;
    if (w[1] < 0x10000 || !r1_safe_read((void *)w[1], n, sizeof n - 1))
        return 0;
    n[sizeof n - 1] = 0;
    if (!sane_name(n))
        return 0;
    snprintf(out, size_out, "%s", n);
    if (parent)
        *parent = w[4];
    if (size_field)
        *size_field = w[6];
    return 1;
}

/* DTI address from a vtable pointer: vtable[4] must be `B8 imm32 C3` */
static unsigned dti_of_vtable(unsigned vt)
{
    unsigned fn;
    unsigned char c[8];
    if (vt < 0x10000 || (vt & 3) || !r1_safe_read((void *)(vt + 16), &fn, 4) || fn < 0x10000 || !r1_safe_read((void *)fn, c, 6))
        return 0;
    if (c[0] == 0xB8 && c[5] == 0xC3)
        return *(unsigned *)(c + 1);
    return 0;
}

int classes_chain(unsigned dti, char *out, int size)
{
    int o = 0;
    for (int depth = 0; dti && depth < 24; depth++) {
        char n[84];
        unsigned parent = 0, sf = 0;
        if (!dti_name(dti, n, sizeof n, &parent, &sf)) {
            o += snprintf(out + o, size - o, "%s?%08x", depth ? " : " : "", dti);
            break;
        }
        o += snprintf(out + o, size - o, "%s%s(%x)", depth ? " : " : "", n, (sf & 0x7FFFFF));
        dti = parent;
    }
    return o;
}

void classes_name(unsigned obj, char *out, int size)
{
    unsigned vt;
    if (!r1_safe_read((void *)obj, &vt, 4)) {
        snprintf(out, size, "unreadable %08x", obj);
        return;
    }
    unsigned d = dti_of_vtable(vt);
    if (!d) {
        unsigned fn = 0;
        unsigned char c[8] = {0};
        r1_safe_read((void *)(vt + 16), &fn, 4);
        r1_safe_read((void *)fn, c, 8);
        snprintf(out, size, "no DTI: vtable %08x (exe+%x), vtable[4]=%08x bytes %02x %02x %02x %02x %02x %02x", vt, vt - 0x400000, fn, c[0], c[1], c[2], c[3], c[4], c[5]);
        return;
    }
    int o = snprintf(out, size, "vtable exe+%x, DTI %08x: ", vt - 0x400000, d);
    classes_chain(d, out + o, size - o);
}

void classes_dtiname(unsigned dti, char *out, int size)
{
    classes_chain(dti, out, size);
}

/* ---- object scan ---- */
typedef struct { unsigned vt, dti; int count; unsigned first[6]; char name[64]; unsigned parent; } cls_t;
static cls_t *cls;
static int n_cls;
#define HT 65536
static int ht[HT];   /* vtable -> index+1 in cls, or -1 = not a class vtable */

static unsigned neg_vt[HT / 2];
static int n_neg;

static int vt_classify(unsigned vt)
{
    unsigned h = (vt >> 2) * 2654435761u >> 16 & (HT - 1);
    for (int probe = 0; probe < HT; probe++, h = (h + 1) & (HT - 1)) {
        int e = ht[h];
        if (e == 0) {
            unsigned d = dti_of_vtable(vt);
            char n[84];
            unsigned parent = 0;
            if (d && dti_name(d, n, sizeof n, &parent, NULL) && n_cls < 20000) {
                cls[n_cls].vt = vt, cls[n_cls].dti = d, cls[n_cls].count = 0, cls[n_cls].parent = parent;
                snprintf(cls[n_cls].name, sizeof cls[n_cls].name, "%s", n);
                n_cls++;
                ht[h] = n_cls;
                return n_cls;
            }
            if (n_neg < (int)(sizeof neg_vt / sizeof neg_vt[0])) {
                neg_vt[n_neg++] = vt;
                ht[h] = -n_neg;
            }
            return -1;
        }
        if (e > 0 && cls[e - 1].vt == vt)
            return e;
        if (e < 0 && neg_vt[-e - 1] == vt)
            return -1;
    }
    return -1;
}

static int cmp_cls(const void *a, const void *b)
{
    const cls_t *x = a, *y = b;
    return y->count - x->count;
}

static int readonly_name(const char *f) { return !strcmp(f, "@read"); }

void classes_scan(const char *file, const char *filter, unsigned scan_lo, unsigned scan_hi, char *out, int size)
{
    BYTE *exe = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    unsigned lo = (unsigned)exe, hi = lo + nt->OptionalHeader.SizeOfImage;   /* a vtable is anywhere inside the image (.rdata) */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    free(cls);
    cls = calloc(20000, sizeof(cls_t));
    memset(ht, 0, sizeof ht);
    n_cls = n_neg = 0;
    FILE *objs = NULL;
    char objfile[300];
    if (filter && filter[0] && !readonly_name(filter)) {
        snprintf(objfile, sizeof objfile, "%s.objects.txt", file);
        objs = fopen(objfile, "w");
    }
    static unsigned char buf[1 << 20];
    int readonly = filter && !strcmp(filter, "@read");   /* lab: only read the memory, parse nothing (crash bisect) */
    unsigned long long total = 0;
    unsigned long hits = 0, regions = 0;
    unsigned char *p = (unsigned char *)(scan_lo > (unsigned)si.lpMinimumApplicationAddress ? scan_lo : (unsigned)si.lpMinimumApplicationAddress);
    unsigned hi_limit = scan_hi && scan_hi < (unsigned)si.lpMaximumApplicationAddress ? scan_hi : (unsigned)si.lpMaximumApplicationAddress;
    DWORD t0 = GetTickCount();
    while ((unsigned)p < hi_limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi))
            break;
        unsigned char *next = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        /* plain read/write private memory only: no guard pages (stacks), no write-combined / uncached mappings (D3D buffers, driver memory) */
        int ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & 0xFFFFFF00u) == 0 && mbi.Protect == PAGE_READWRITE;
        if (ok && (unsigned char *)mbi.BaseAddress >= (unsigned char *)0x10000) {
            regions++;
            if ((regions & 63) == 0)
                r1_log("objscan: region %lu at %p, %llu bytes, %lu objects so far, %d vtables", regions, mbi.BaseAddress, total, hits, n_cls);
            for (unsigned char *q = mbi.BaseAddress; q < next; q += sizeof buf) {
                SIZE_T want = (SIZE_T)(next - q < (ptrdiff_t)sizeof buf ? next - q : (ptrdiff_t)sizeof buf), got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), q, buf, want, &got) || got < 4)
                    continue;
                total += got;
                if (readonly)
                    continue;
                for (SIZE_T i = 0; i + 4 <= got; i += 4) {
                    unsigned v = *(unsigned *)(buf + i);
                    if (v < lo || v >= hi)
                        continue;
                    int e = vt_classify(v);
                    if (e <= 0)
                        continue;
                    cls_t *c = &cls[e - 1];
                    if (c->count < 6)
                        c->first[c->count] = (unsigned)q + (unsigned)i;
                    c->count++;
                    hits++;
                    if (objs && strstr(c->name, filter))
                        fprintf(objs, "%08x %s\n", (unsigned)q + (unsigned)i, c->name);
                }
            }
        }
        if (next <= p)
            break;
        p = next;
    }
    if (objs)
        fclose(objs);
    qsort(cls, n_cls, sizeof(cls_t), cmp_cls);
    FILE *f = fopen(file, "w");
    if (f) {
        fprintf(f, "# objscan: %llu bytes in %lu regions, %lu objects with a class vtable, %d vtables, %lu ms\n", total, regions, hits, n_cls, GetTickCount() - t0);
        fprintf(f, "# count class vtable(exe+rva) parent first-addresses\n");
        for (int i = 0; i < n_cls; i++) {
            char pn[84] = "-";
            if (cls[i].parent)
                dti_name(cls[i].parent, pn, sizeof pn, NULL, NULL);
            fprintf(f, "%d %s exe+%x %s", cls[i].count, cls[i].name, cls[i].vt - lo, pn);
            for (int k = 0; k < 6 && k < cls[i].count; k++)
                fprintf(f, " %08x", cls[i].first[k]);
            fputc('\n', f);
        }
        fclose(f);
    }
    snprintf(out, size, "scanned %llu bytes in %lu regions, %lu objects, %d vtables with a class, %lu ms -> %s%s%s", total, regions, hits, n_cls, GetTickCount() - t0,
             f ? file : "(cannot write)", objs ? " and " : "", objs ? objfile : "");
}
