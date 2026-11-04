/*
 * gmdtext.h - reader for the game's own message files (nativePC\msg\*.gmd, MT Framework GMD v0x10201 as used by Revelations 1): the lobby quotes the stock words
 * (character names, Continue / Load Game / New Game / Campaign) in the language the game runs in instead of embedding them. Pure C, no game dependencies.
 * Layout: "GMD\0", version, language, 5 counts at 0x14 (labels, messages, label bytes, message bytes, name bytes), name + NUL at 0x28, one 8-byte record per label
 * (message index, label hash), the NUL separated label strings, the NUL separated message strings.
 */
#ifndef GMDTEXT_H
#define GMDTEXT_H

typedef struct gmd gmd_t;
gmd_t *gmd_parse(const void *data, unsigned size);   /* copies the data; NULL when the file is not a GMD of this layout */
gmd_t *gmd_load(const char *path);                   /* whole file, max 1 MB */
void gmd_free(gmd_t *g);
int gmd_count(const gmd_t *g);                       /* number of messages */
const char *gmd_by_index(const gmd_t *g, int index); /* NULL out of range */
const char *gmd_by_label(const gmd_t *g, const char *label);   /* NULL when there is no such label */

#endif
