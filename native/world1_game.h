/*
 * world1_game.h - the game glue of the shared world: hangs the engine hooks (world1_hooks.c) on the world model (world1.c / world1_net.c).
 * Off by default: a single-player game and the split screen run on the original bytes. The hooks are written when a network session has STARTED
 * (net1_session_active), with REV1_WORLD=1 (hooks only observe: counters and log, no model) or by the console (`world apply`).
 */
#ifndef WORLD1_GAME_H
#define WORLD1_GAME_H

/* once from probe_install (after guards_init): registers the frame callback; nothing is written into the game yet */
int world_init(void);
/* console `world status | apply | revert | auto | list | flags on|off | set block 0|1` (game thread; probe.c) */
void world_cmd(const char *arg, char *out, int size);

/* allep1 (hunk1.h): the class id the engine builds the humans of the NEXT level start from, slot 1 = the second human; id 0 = stop persisting (any thread). The engine rewrites the id at
 * every section start, so a persisted writer puts it back every few ms. Used by the section-start hooks of rolebarrier1_game.c for Hunk in the solo part 16-2. */
void world_player_class(unsigned slot, unsigned id);

/* ep1-solo (solo1.h): readers for the other modules (any thread; 0 / not when the network glue is not running) */
int solo1_watch_now(unsigned *watched, unsigned *enters);   /* 1 = this PC's human is switched off by the story and viewport 0 follows the other human (*watched = his unit) */
int solo1_unit_off(unsigned unit);                          /* 1 = this player unit is switched off by the story here (update + draw bits cleared) */
int solo1_cam_gameplay(unsigned vp0, unsigned mgr0);        /* 1 = viewport 0's camera is the gameplay view (manager 0, or manager 1 while watching) */

#endif
