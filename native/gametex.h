/*
 * gametex.h - reads the game's OWN interface material at run time, from the installed game folder, without shipping any of it:
 *   - ARC v7 archives (MT Framework: "ARC\0", u16 version, u16 count, 80-byte entries {name[64], type, compressed size, size | flags << 29, offset}, zlib per entry),
 *   - TEX textures ("TEX\0" v0x9d: u32 w2 = mips | width << 6 | height << 19, u32 w3 with the format byte at bits 8..15, u32 mip offset table; block-compressed DXT1 / DXT3 / DXT5
 *     or 32-bit BGRA), decoded on the CPU into 0xAARRGGBB pixels,
 *   - GFD font tables ("GFD\0": glyph records of 16 bytes at the end of the file: code point, then x | y * 4 << 10 | page, width | height * 4 << 10 ..., advance).
 * Everything is loaded once, on demand, cached by the caller; any failure returns NULL / 0 and the caller keeps its own drawing. Pure C, no game dependencies, no Direct3D.
 */
#ifndef GAMETEX_H
#define GAMETEX_H

/* whole entry of an archive (inflated), malloc'ed; NULL when the archive or the entry is missing. *size_out = bytes. */
unsigned char *gt_arc_read(const char *arc_path, const char *entry, unsigned *size_out);
/* raw zlib stream (2 byte header) or raw deflate: returns 1 when exactly outsize bytes came out */
int gt_inflate_zlib(const unsigned char *in, unsigned insize, unsigned char *out, unsigned outsize);

typedef struct { int w, h; unsigned *px; } gt_img;   /* 0xAARRGGBB, top-down, malloc'ed px */
int gt_tex_decode(const unsigned char *tex, unsigned size, gt_img *out);   /* 1 on success; DXT1 has alpha 255 */
void gt_img_free(gt_img *im);

/* fonts: a GFD table plus its atlas pages "<page_fmt with %02d>" read from the same archive on demand */
typedef struct {
    unsigned cp;
    unsigned char page, adv;
    unsigned short x, y4, w, h4;   /* pixels in the atlas; y and h in quarter pixels */
} gt_glyph;
typedef struct gt_font gt_font;
gt_font *gt_font_open(const char *arc_path, const char *gfd_entry, const char *page_fmt);
void gt_font_free(gt_font *f);
const gt_glyph *gt_font_find(const gt_font *f, unsigned cp);   /* NULL when the font has no such glyph */
const gt_img *gt_font_page(gt_font *f, int page);               /* decoded atlas page (cached); NULL when missing */
int gt_font_cell(const gt_font *f);                              /* glyph cell height in pixels (the common height of the glyph records) */

#endif
