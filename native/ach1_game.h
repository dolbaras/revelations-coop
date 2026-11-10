#ifndef ACH1_GAME_H
#define ACH1_GAME_H
/* ach1_game.c: the achievement gate of a network co-op game (B10; decisions in ach1.h) */
void ach1_game_init(void);                                  /* once, from probe_install: checks the call sites' bytes and points them at the gate */
void ach1_game_frame(void);                                 /* game thread, every frame: a finished session clears the ledger */
void ach1_game_cmd(const char *arg, char *out, int size);   /* console: ach [status] | gate 0|1 | taint 0|1 | profile */
int ach1_game_own_kill(void);                               /* client: its own kill credited by the kill record alone -> the game's +0x72 setter (1 = credited) */
#endif
