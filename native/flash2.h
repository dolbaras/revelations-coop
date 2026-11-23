/* flash2.h - the second human's flashlight in split screen (see flash2.c) */
#ifndef FLASH2_H
#define FLASH2_H
void flash2_frame(int active, unsigned p2_unit);   /* per logic pass (split_frame): active = the split shows two gameplay views; p2_unit = the second human's unit */
void flash2_viewport(unsigned x, unsigned y, unsigned w, unsigned h, unsigned bw, unsigned bh);   /* d3d.c SetViewport hook (render thread) */
void flash2_set(int on);
int flash2_is_on(void);
void flash2_status(char *out, int size);
#endif
