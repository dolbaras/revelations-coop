/*
 * overlay.h - draws the co-op lobby screen and the title-menu hint over the finished frame, from the Present hook (d3d.c).
 * Technique proven by the Revelations 2 mod (its stage 8): the screen is composed on the CPU with GDI (system fonts, one face per language class) into a DIB and copied into the back
 * buffer with UpdateSurface + StretchRect: no BeginScene, no sprite, no render state touched, nothing at all happens while no scene is set. (D3DX sprites / fonts stalled
 * the game's render thread there; do not use them.) Opaque only: StretchRect does not blend.
 * Layout is done in a virtual 1920x1080 space (16:9, centred on wider windows) and scaled to the back buffer.
 * The lobby is not drawn on a flat colour: when it opens, the finished game frame is read back once (GetRenderTargetData), blurred, graded to the menu's teal and darkened with a vignette,
 * and that picture is the backdrop (the lobby stays a still image; without a readback it falls back to a plain teal-black vignette). Hint strip: opaque, as before.
 */
#ifndef OVERLAY_H
#define OVERLAY_H

#include "lobby1.h"

enum { OV_NONE = 0, OV_LOBBY = 1, OV_HINT = 2 };

typedef struct {
    int kind;            /* OV_* */
    l1_view v;           /* OV_LOBBY */
    char hint[300];      /* OV_HINT: one line at the bottom edge */
    int lang;            /* 0..12, picks the font face */
    unsigned tick_ms;    /* animation clock */
    int cursor_on, cursor_x, cursor_y;   /* mouse pointer in virtual 1920x1080 coordinates (drawn while the mouse is in use: the game hides its own cursor on some screens) */
} ov_scene;

void overlay_draw(void *d3d9_device, const ov_scene *sc);   /* inside the Present hook */
void overlay_set_hint_rect(float x, float y, float w, float h);   /* where the hint strip sits (virtual coordinates) */
void overlay_release(void);                                  /* before IDirect3DDevice9::Reset (and at shutdown) */
int overlay_row_at_client(int px, int py, int client_w, int client_h, int *vx, int *vy);   /* row of the last drawn lobby under a client pixel, -1 none */
unsigned overlay_frames(void);                               /* frames copied so far */
void overlay_stats(char *out, int size);
/* offline: render a scene into a 32-bit top-down bitmap of w x h pixels without any device (tests/overlay_preview.c); returns 0 on failure */
int overlay_render_to_bitmap(const ov_scene *sc, int w, int h, unsigned **pixels_out);
void overlay_set_game_archive(const char *resident_arc);   /* where the game's own fonts / sprites are read from (default: <exe folder>\nativePC\archive\gui\resident.arc); offline preview sets it */
void overlay_set_preview_backdrop(const unsigned *px, int w, int h);   /* offline: the picture that stands for the game frame behind the lobby (32-bit top-down, may be NULL) */

#endif
