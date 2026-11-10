/* downed1.h - engine glue of the "downed / revive" rule (Revelations 1, local split). Default OFF: see downed1.c. */
#ifndef DOWNED1_H
#define DOWNED1_H

/* once, from probe_install (game thread). Returns 1 when the rule is switched on (REV1_DOWNED=1); nothing is hooked before the first logic pass. */
int downed1_init(void);

/* console (logic thread): downed [status] | down <k> | pose <k> down|keep|up|die | god 0|1 (LAB ONLY) */
void downed_cmd(const char *arg, char *out, int size);

/* the 8 bytes downed1 wrote at Damage 0x6CA830 (jmp hook; nop x3): 1 and the bytes while its hook is in, else 0 (world1_game.c accepts the call site with them) */
int downed1_damage_patch(unsigned char out[8]);

/* network mode: 1 while this PC's own human lies DOWN (puppet_game.c sends CP_SF_DOWN in its pad / STATE flags, which holds the partner's puppet of him); 0 otherwise / split */
int downed1_own_down(void);

#endif
