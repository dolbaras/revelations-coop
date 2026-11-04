#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gmdtext.h"

struct gmd {
    unsigned char *data;
    unsigned size;
    int nlabels, nmsgs;
    const char **label;   /* label i */
    int *label_msg;       /* message index of label i */
    const char **msg;
};

static unsigned rd(const unsigned char *p, unsigned off) { unsigned v; memcpy(&v, p + off, 4); return v; }

gmd_t *gmd_parse(const void *data, unsigned size)
{
    const unsigned char *d = data;
    if (!d || size < 0x30 || memcmp(d, "GMD\0", 4) != 0) return NULL;
    unsigned lc = rd(d, 0x14), sc = rd(d, 0x18), ls = rd(d, 0x1C), ss = rd(d, 0x20), ns = rd(d, 0x24);
    if (lc > 20000 || sc > 20000 || ns > 200) return NULL;
    unsigned long long need = 0x28ull + ns + 1 + 8ull * lc + ls + ss;
    if (need > size) return NULL;
    gmd_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->data = malloc(size + 2);
    if (!g->data) { free(g); return NULL; }
    memcpy(g->data, d, size);
    g->data[size] = g->data[size + 1] = 0;
    g->size = size;
    g->nlabels = (int)lc, g->nmsgs = (int)sc;
    g->label = calloc(lc ? lc : 1, sizeof(char *));
    g->label_msg = calloc(lc ? lc : 1, sizeof(int));
    g->msg = calloc(sc ? sc : 1, sizeof(char *));
    if (!g->label || !g->label_msg || !g->msg) { gmd_free(g); return NULL; }
    unsigned off = 0x28 + ns + 1;
    for (unsigned i = 0; i < lc; i++) g->label_msg[i] = (int)rd(g->data, off + 8 * i);
    off += 8 * lc;
    const char *p = (const char *)g->data + off, *end = p + ls;
    for (unsigned i = 0; i < lc; i++) {
        g->label[i] = p;
        while (p < end && *p) p++;
        if (p < end) p++;
    }
    off += ls;
    p = (const char *)g->data + off, end = p + ss;
    for (unsigned i = 0; i < sc; i++) {
        g->msg[i] = p;
        while (p < end && *p) p++;
        if (p < end) p++;
    }
    return g;
}

gmd_t *gmd_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) { fclose(f); return NULL; }
    unsigned char *buf = malloc((size_t)n);
    gmd_t *g = NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) g = gmd_parse(buf, (unsigned)n);
    free(buf);
    fclose(f);
    return g;
}

void gmd_free(gmd_t *g)
{
    if (!g) return;
    free(g->data), free(g->label), free(g->label_msg), free(g->msg);
    free(g);
}

int gmd_count(const gmd_t *g) { return g ? g->nmsgs : 0; }

const char *gmd_by_index(const gmd_t *g, int index)
{
    if (!g || index < 0 || index >= g->nmsgs) return NULL;
    return g->msg[index];
}

const char *gmd_by_label(const gmd_t *g, const char *label)
{
    if (!g || !label) return NULL;
    for (int i = 0; i < g->nlabels; i++)
        if (g->label[i] && !strcmp(g->label[i], label)) return gmd_by_index(g, g->label_msg[i]);
    return NULL;
}
