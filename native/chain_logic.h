/*
 * chain_logic.h: what the proxy does with a dinput8_real.dll next to it (the chain rule, proxy.c load_real_dinput8), decided from the file's bytes
 * before any code of it runs. Pure logic (no Windows calls), so tests\test_chain.c checks it on made-up images and on real files.
 *
 *   CHAIN_LOAD     load it and forward DirectInput8Create to it (a wrapper that loads the system dinput8.dll by that fixed name works this way)
 *   CHAIN_SKIP_UAL an Ultimate ASI Loader (it exports IsUltimateASILoader): it knows itself only by its own file name, and under the name
 *                  dinput8_real.dll it shows "This library isn't supported." and calls ExitProcess(0) (UAL source, proxy::Load of v10.0.2,
 *                  LoadOriginalLibrary of v7.8.0 = the Fusion Fix's copy), so loading it would close the game. The co-op installer runs such a
 *                  loader as winmm.dll instead (a name it serves and rerev.exe imports).
 *   CHAIN_SKIP_COOP a copy of this mod (it holds the proxy's mark): named dinput8_real.dll it would find itself as its own dinput8_real.dll and
 *                  forward DirectInput8Create to itself without end, and every hook would be installed twice.
 */
#ifndef CHAIN_LOGIC_H
#define CHAIN_LOGIC_H

#include <stddef.h>
#include <string.h>

enum { CHAIN_LOAD = 0, CHAIN_SKIP_UAL = 1, CHAIN_SKIP_COOP = 2 };

static unsigned chain_u16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long chain_u32(const unsigned char *p) { return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }

/* The file offset of an RVA by the section table, or -1. */
static long chain_rva_off(const unsigned char *b, size_t n, size_t sec, unsigned nsec, unsigned long rva)
{
    for (unsigned i = 0; i < nsec; i++) {
        size_t s = sec + 40 * (size_t)i;
        if (s + 40 > n)
            return -1;
        unsigned long va = chain_u32(b + s + 12), vsize = chain_u32(b + s + 8), raw = chain_u32(b + s + 16), rawptr = chain_u32(b + s + 20);
        unsigned long span = vsize > raw ? vsize : raw;
        if (rva >= va && rva < va + span) {
            unsigned long off = rva - va;
            if (off >= raw)
                return -1;   /* in the zero-filled tail: not in the file */
            if ((size_t)rawptr + off >= n)
                return -1;
            return (long)(rawptr + off);
        }
    }
    return -1;
}

/* 1 when the PE image in b[0..n) (a file's bytes, not a mapped image) exports a function of that name; 0 otherwise, also for anything that is not a PE. */
static int chain_pe_exports(const unsigned char *b, size_t n, const char *name)
{
    if (!b || n < 0x40 || b[0] != 'M' || b[1] != 'Z')
        return 0;
    unsigned long pe = chain_u32(b + 0x3C);
    if (pe > n || n - pe < 24 + 2 || memcmp(b + pe, "PE\0\0", 4) != 0)
        return 0;
    unsigned nsec = chain_u16(b + pe + 6), optsize = chain_u16(b + pe + 20);
    size_t opt = pe + 24, sec = opt + optsize;
    if (opt + optsize > n)
        return 0;
    unsigned magic = chain_u16(b + opt);
    size_t dirs = magic == 0x10b ? opt + 96 : magic == 0x20b ? opt + 112 : 0;
    if (!dirs || dirs + 8 > opt + optsize)
        return 0;
    unsigned long erva = chain_u32(b + dirs), esize = chain_u32(b + dirs + 4);
    if (!erva || !esize)
        return 0;
    long ed = chain_rva_off(b, n, sec, nsec, erva);
    if (ed < 0 || (size_t)ed + 40 > n)
        return 0;
    unsigned long nnames = chain_u32(b + ed + 24), names_rva = chain_u32(b + ed + 32);
    long names = chain_rva_off(b, n, sec, nsec, names_rva);
    if (names < 0 || nnames > 65536)
        return 0;
    size_t want = strlen(name);
    for (unsigned long i = 0; i < nnames; i++) {
        if ((size_t)names + 4 * (i + 1) > n)
            return 0;
        long s = chain_rva_off(b, n, sec, nsec, chain_u32(b + names + 4 * i));
        if (s < 0 || (size_t)s + want + 1 > n)
            continue;
        if (memcmp(b + s, name, want) == 0 && b[s + want] == 0)
            return 1;
    }
    return 0;
}

/* 1 when the bytes hold the text (the proxy's mark "RE Revelations co-op proxy"). */
static int chain_has_text(const unsigned char *b, size_t n, const char *text)
{
    size_t k = strlen(text);
    if (!b || !k || n < k)
        return 0;
    for (size_t i = 0; i + k <= n; i++)
        if (b[i] == (unsigned char)text[0] && memcmp(b + i, text, k) == 0)
            return 1;
    return 0;
}

/* The verdict for a dinput8_real.dll whose bytes are b[0..n). */
static int chain_judge(const unsigned char *b, size_t n, const char *coop_mark)
{
    if (chain_has_text(b, n, coop_mark))
        return CHAIN_SKIP_COOP;
    if (chain_pe_exports(b, n, "IsUltimateASILoader"))
        return CHAIN_SKIP_UAL;
    return CHAIN_LOAD;
}

#endif
