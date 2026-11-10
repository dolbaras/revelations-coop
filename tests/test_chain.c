/*
 * test_chain.c: the chain rule's verdict on a dinput8_real.dll (native/chain_logic.h, used by proxy.c load_real_dinput8).
 * Made-up PE images (32- and 64-bit, export names in another section, broken headers) and, from the command line, real files:
 *   test_chain.exe [--load <file>]... [--ual <file>]... [--coop <file>]...
 * tests/build.sh passes the system's 32-bit dinput8.dll (--load) and the mod's own build (--coop) when they exist; the two Ultimate ASI Loader builds
 * (v10.0.2 and the Fusion Fix's v7.8.0) are given with UAL_FILES (they are not in the repository). (2026-11-07: the Fusion Fix's .asi, which holds the
 * text "IsUltimateASILoader" for its own GetProcAddress but exports only InitializeASI, is judged CHAIN_LOAD: the export table decides, not the text.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/chain_logic.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { put16(p, (unsigned)(v & 0xFFFF)), put16(p + 2, (unsigned)(v >> 16)); }

/* A DLL image as a file: DOS header, PE header, optional header (PE32 or PE32+), two sections (.text 0x1000, .edata at edata_rva), the export
 * directory with the given names in .edata. Returns the size. */
static size_t make_pe(unsigned char *b, size_t cap, int pe64, const char **names, int nnames, unsigned long edata_rva)
{
    memset(b, 0, cap);
    b[0] = 'M', b[1] = 'Z';
    put32(b + 0x3C, 0x80);
    unsigned char *pe = b + 0x80;
    memcpy(pe, "PE\0\0", 4);
    put16(pe + 4, pe64 ? 0x8664 : 0x14C);
    put16(pe + 6, 2);
    unsigned optsize = pe64 ? 0xF0 : 0xE0;
    put16(pe + 20, optsize);
    put16(pe + 22, 0x2102);
    unsigned char *opt = pe + 24;
    put16(opt, pe64 ? 0x20B : 0x10B);
    unsigned char *dirs = opt + (pe64 ? 112 : 96);
    put32(opt + (pe64 ? 108 : 92), 16);   /* NumberOfRvaAndSizes */
    put32(dirs, edata_rva);
    put32(dirs + 4, 0x100);
    unsigned char *sec = opt + optsize;
    memcpy(sec, ".text", 5);
    put32(sec + 8, 0x200), put32(sec + 12, 0x1000), put32(sec + 16, 0x200), put32(sec + 20, 0x400);
    memcpy(sec + 40, ".edata", 6);
    put32(sec + 48, 0x400), put32(sec + 52, edata_rva), put32(sec + 56, 0x400), put32(sec + 60, 0x600);
    /* .edata at file 0x600: directory, then the name pointers, then the strings */
    unsigned char *ed = b + 0x600;
    put32(ed + 20, (unsigned long)nnames), put32(ed + 24, (unsigned long)nnames);
    put32(ed + 32, edata_rva + 0x40);
    unsigned long str = 0x100;
    for (int i = 0; i < nnames; i++) {
        put32(ed + 0x40 + 4 * i, edata_rva + str);
        strcpy((char *)ed + str, names[i]);
        str += (unsigned long)strlen(names[i]) + 1;
    }
    return 0xA00;
}

static unsigned char *read_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = len > 0 ? (unsigned char *)malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len)
        free(b), b = NULL;
    fclose(f);
    *n = b ? (size_t)len : 0;
    return b;
}

static const char *MARK = "RE Revelations co-op proxy";

int main(int argc, char **argv)
{
    static unsigned char b[0x1000];
    size_t n;

    printf("-- made-up images\n");
    const char *ual[] = {"DirectInput8Create", "IsUltimateASILoader", "timeGetTime"};
    const char *plain[] = {"DirectInput8Create", "DllCanUnloadNow"};
    const char *near[] = {"IsUltimateASILoaderX", "XIsUltimateASILoader", "IsUltimateASI"};
    n = make_pe(b, sizeof b, 0, ual, 3, 0x2000);
    CHECK(chain_pe_exports(b, n, "IsUltimateASILoader"));
    CHECK(chain_pe_exports(b, n, "timeGetTime"));
    CHECK(!chain_pe_exports(b, n, "DrawDibDraw"));
    CHECK(chain_judge(b, n, MARK) == CHAIN_SKIP_UAL);
    n = make_pe(b, sizeof b, 1, ual, 3, 0x2000);   /* a 64-bit build reads the same (the loader would fail to map it anyway) */
    CHECK(chain_judge(b, n, MARK) == CHAIN_SKIP_UAL);
    n = make_pe(b, sizeof b, 0, plain, 2, 0x2000);
    CHECK(chain_pe_exports(b, n, "DirectInput8Create"));
    CHECK(chain_judge(b, n, MARK) == CHAIN_LOAD);
    n = make_pe(b, sizeof b, 0, near, 3, 0x2000);   /* names that only start or end like it */
    CHECK(chain_judge(b, n, MARK) == CHAIN_LOAD);
    /* the name only in a string (a plugin that asks GetProcAddress for it), not exported: not a loader */
    n = make_pe(b, sizeof b, 0, plain, 2, 0x2000);
    strcpy((char *)b + 0x480, "IsUltimateASILoader");
    CHECK(chain_judge(b, n, MARK) == CHAIN_LOAD);
    /* the mod's mark anywhere: a copy of the mod, also when it exports the loader's name */
    n = make_pe(b, sizeof b, 0, ual, 3, 0x2000);
    strcpy((char *)b + 0x480, "RE Revelations co-op proxy %s loaded");
    CHECK(chain_judge(b, n, MARK) == CHAIN_SKIP_COOP);
    /* broken files: never a loader, never a crash */
    n = make_pe(b, sizeof b, 0, ual, 3, 0x2000);
    put32(b + 0x3C, 0x7FFFFFF0);
    CHECK(chain_judge(b, n, MARK) == CHAIN_LOAD);
    n = make_pe(b, sizeof b, 0, ual, 3, 0x2000);
    put32(b + 0x80 + 24 + 96, 0x9000);   /* the export RVA outside every section */
    CHECK(!chain_pe_exports(b, n, "IsUltimateASILoader"));
    n = make_pe(b, sizeof b, 0, ual, 3, 0x2000);
    put32(b + 0x600 + 24, 0x7FFFFFFF);   /* a huge name count */
    CHECK(!chain_pe_exports(b, n, "IsUltimateASILoader"));
    n = make_pe(b, sizeof b, 0, ual, 3, 0x2000);
    CHECK(!chain_pe_exports(b, 0x700, "IsUltimateASILoader"));   /* cut off before the strings */
    CHECK(chain_judge((const unsigned char *)"ASI LOADER of somebody else", 27, MARK) == CHAIN_LOAD);
    CHECK(chain_judge(b, 0, MARK) == CHAIN_LOAD);
    CHECK(chain_judge(NULL, 0, MARK) == CHAIN_LOAD);

    printf("-- real files\n");
    int real = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        int want = !strcmp(argv[i], "--ual") ? CHAIN_SKIP_UAL : !strcmp(argv[i], "--coop") ? CHAIN_SKIP_COOP : !strcmp(argv[i], "--load") ? CHAIN_LOAD : -1;
        unsigned char *f = read_file(argv[i + 1], &n);
        if (want < 0 || !f) {
            printf("FAIL cannot use %s %s\n", argv[i], argv[i + 1]);
            fails++, checks++;
            continue;
        }
        int v = chain_judge(f, n, MARK);
        printf("%s %s: verdict %d (want %d), exports IsUltimateASILoader %d, DirectInput8Create %d\n", v == want ? "ok  " : "FAIL", argv[i + 1], v, want,
               chain_pe_exports(f, n, "IsUltimateASILoader"), chain_pe_exports(f, n, "DirectInput8Create"));
        checks++;
        if (v != want)
            fails++;
        if (want == CHAIN_SKIP_UAL)   /* a real loader serves dinput8.dll: the export parse found its names, not only the one asked for */
            CHECK(chain_pe_exports(f, n, "DirectInput8Create"));
        real++;
        free(f);
    }
    printf("%d real file(s)\n", real);
    printf("test_chain: %d of %d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
