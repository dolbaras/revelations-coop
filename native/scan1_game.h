/* scan1_game.h - the engine glue of the Genesis scanner for the second player (scan1.c; the rules: scan1.h, notes/scan1.md) */
#ifndef SCAN1_GAME_H
#define SCAN1_GAME_H

/* once, from probe_install (game thread): registers the per-pass callback; the hidden-item hook is set at the first pass when the stock bytes match
 * (REV1_SCAN=0 keeps the whole module off) */
int scan1_init(void);

/* split.c's exchange of the second human (ops SPLIT_XG_* of split.h), game thread */
int scan1_split_hook(int op);

/* story1_game.inc (E2): 1 = the analysis rate is one team value here (1-2, or the module is off), 0 = each PC keeps its own human's rate. Game thread. */
int scan1_rate_shared_now(void);

/* console (logic thread): scan [status] | who | items | on | off | x 0|1 | give | p2rate <v> | reveal <item hex> */
void scan_cmd(const char *arg, char *out, int size);

#endif
