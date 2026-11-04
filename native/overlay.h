/*
 * overlay.h - draws the co-op lobby screen and the title-menu hint over the finished frame, from the Present hook (d3d.c).
 * Technique proven by the Revelations 2 mod (its stage 8): the screen is composed on the CPU with GDI (system fonts, one face per language class) into a DIB and copied into the back
 * buffer with UpdateSurface + StretchRect: no BeginScene, no sprite, no render state touched, nothing at all happens while no scene is set. (D3DX sprites / fonts stalled
 * the game's render thread there; do not use them.) Opaque only: StretchRect does not blend.
 * Layout is done in a virtual 1920x1080 space (16:9, centred on wider windows) and scaled to the back buffer.
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
} ov_scene;

void overlay_draw(void *d3d9_device, const ov_scene *sc);   /* inside the Present hook */
void overlay_set_hint_rect(float x, float y, float w, float h);   /* where the hint strip sits (virtual coordinates) */
void overlay_release(void);                                  /* before IDirect3DDevice9::Reset (and at shutdown) */
int overlay_row_at_client(int px, int py, int client_w, int client_h, int *vx, int *vy);   /* row of the last drawn lobby under a client pixel, -1 none */
unsigned overlay_frames(void);                               /* frames copied so far */
void overlay_stats(char *out, int size);
/* offline: render a scene into a 32-bit top-down bitmap of w x h pixels without any device (tests/overlay_preview.c); returns 0 on failure */
int overlay_render_to_bitmap(const ov_scene *sc, int w, int h, unsigned **pixels_out);

#endif
