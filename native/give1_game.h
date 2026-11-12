/* give1_game.h - the engine glue of "give to partner" (give1.c; the rule: give1.h, notes/give1.md) */
#ifndef GIVE1_GAME_H
#define GIVE1_GAME_H

/* once, from probe_install (game thread): registers the per-pass callback; the item screen's update is wrapped at the first pass when every stock byte matches
 * (REV1_GIVE=0 keeps it off) */
int give1_init(void);

/* console (logic thread): give [status] | on | off | gui | bag [slot] | lab <slot> <tab> <id> | fill <slot> herb|ammo|gren <index> <n> | set reach <units> | sound <ok> <err> */
void give_cmd(const char *arg, char *out, int size);

#endif
