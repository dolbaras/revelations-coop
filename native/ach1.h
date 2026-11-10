/*
 * ach1.h: Revelations 1 co-op, per-player achievements (B10). Pure data and decisions (no game access, not hooked into the engine yet): the DLL glue will call
 * ach1_gate() from the setters' hook points, the offline tests (tests\test_ach1.c) run the same code against a model of two PCs.
 * Evidence for every row: notes\ach-episode1.md and notes\achievements-map.md (static, build 13298487, nothing was run).
 *
 * How Revelations 1 awards an achievement (differs from Revelations 2, which counts stat kinds):
 *  - each achievement is a cAchievementInfo polled every frame (cAchievementInfo_update 0x86D750). It evaluates cAchievementInfo_CheckCondition 0x86D220 over LOCAL state only
 *    (the player profile object [0x116B3A4], the save flags [0x116A344], the Episode records [0x116A7E4]); a true result sets the "sent" bit in the save and queues
 *    ISteamUserStats::SetAchievement. The game never sends an achievement over the network and the mod must not either.
 *  - so "each PC credits its own human" is decided entirely by WHICH calls of the profile setters (0x499260, 0x499320, 0x4993E0, 0x499530, 0x4996D0, 0x4997B0, 0x4998D0,
 *    0x499960, 0x4999F0, 0x499200) run on this PC. The setters are called by game code that does not know about the partner: a puppet of the partner's human (a real player-class
 *    unit driven by the partner's pad), a host-side enemy killed by the client, a replayed death all reach the same setters. ach1_gate() is the filter placed in front of them.
 *
 * Co-op rule (owner, RE5/RE6 style, as in Revelations 2): every PC runs its own game and credits ITS human only; nothing is credited for the partner; story and save state
 * (episode clears, scenario flags, item flags) are reached by every PC through its own copy of the synced world.
 */
#ifndef ACH1_H
#define ACH1_H

#include <string.h>

/* ---- the 50 ids: how each behaves in co-op, and whether Episode 1 alone can reach it ---------------------------------------------------------------------------------------- */
enum {                       /* rule */
    A1R_ACTOR = 1,           /* a counter / bit fed by what one human does (kills, ladder, tech, explosion): only the PC of that human credits it */
    A1R_SCAN,                /* Genesis scan: only the scanner's PC */
    A1R_FLAGS,               /* save flags set by scenario FSMs and item pickups: every PC reaches them through its own synced world (idempotent) */
    A1R_CAMPAIGN,            /* episode clear records: every PC records the clear itself when it reaches the end of the episode, at the session difficulty */
    A1R_NA                   /* Raid (sCoopManager): the mod never runs in Raid */
};
enum { A1X_NO = 0, A1X_MAYBE, A1X_YES };   /* reachable inside Episode 1 alone (not via preloaded profile values, see ach-episode1.md) */

typedef struct {
    unsigned char id;        /* the game's id 1..50 (Steam name = snprintf("%d", id) unless the net object sets another format: confirm live) */
    unsigned char rule;
    unsigned char ep1;
    unsigned char setter;    /* ach1_setter_t.sid that feeds it (0 = none: save flags / episode records) */
    const char *cond;        /* the game's condition (CheckCondition case), short */
} ach1_entry_t;

enum {                       /* setter ids, index into ach1_setters */
    A1S_NONE = 0,
    A1S_C70,                 /* profile +0x70 counter, cap 100: ladder climb states (id 23) */
    A1S_C71,                 /* profile +0x71 counter, cap 100: charge-action tech / weapon-explode kinds 8,9 (id 24) */
    A1S_C72,                 /* profile +0x72 counter, cap 150: an enemy died (rank hook uEnemyBase_vf43 -> 0x899CA0), no killer check (id 14) */
    A1S_C74,                 /* profile +0x74 counter, cap 100: zombie hit path 0x5B8530 (id 17) */
    A1S_SCAN,                /* profile +0x6C bit mask of 17 scanned enemy kinds (Genesis scan, 0x8BA5A0) (ids 12, 13) */
    A1S_B3C_4,               /* profile +0x3C bit 4: uWpBoneArrow hit (id 15) */
    A1S_B3C_6,               /* bit 6: Scagdead dying state (id 18) */
    A1S_B3C_7,               /* bit 7: uWeaponExplode effect, three or more targets (id 19) */
    A1S_B3C_8,               /* bit 8: uScarmiglione path (id 20) */
    A1S_B44_0,               /* profile +0x44 bit 0, upper-body state 0x6B95E0 (id 21, condition inverted: true while the bit is clear) */
    A1S_N
};

#define ACH1_N 50
static const ach1_entry_t ach1_table[ACH1_N] = {
    {1, A1R_CAMPAIGN, A1X_NO, 0, "episodes 1-3 each cleared on any difficulty (0x899490)"},
    {2, A1R_CAMPAIGN, A1X_NO, 0, "episodes 4-6"},
    {3, A1R_CAMPAIGN, A1X_NO, 0, "episodes 7-9"},
    {4, A1R_CAMPAIGN, A1X_NO, 0, "episodes 10-12"},
    {5, A1R_CAMPAIGN, A1X_MAYBE, 0, "profile +0x3C bit 0, or the bits of 6"},
    {6, A1R_CAMPAIGN, A1X_MAYBE, 0, "profile +0x3C bit 1 or bit 2"},
    {7, A1R_CAMPAIGN, A1X_MAYBE, 0, "profile +0x3C bit 2"},
    {8, A1R_CAMPAIGN, A1X_NO, 0, "id 6 condition and Episode counter 0x89D490 == 0"},
    {9, A1R_FLAGS, A1X_MAYBE, 0, "count of save flags 0x120..0x13D >= target (1st threshold)"},
    {10, A1R_FLAGS, A1X_MAYBE, 0, "same count, 2nd threshold"},
    {11, A1R_FLAGS, A1X_NO, 0, "same count, 3rd threshold"},
    {12, A1R_SCAN, A1X_MAYBE, A1S_SCAN, "bits set in profile +0x6C >= target"},
    {13, A1R_SCAN, A1X_NO, A1S_SCAN, "all 17 scan kinds"},
    {14, A1R_ACTOR, A1X_YES, A1S_C72, "profile +0x72 >= target (cap 150)"},
    {15, A1R_ACTOR, A1X_NO, A1S_B3C_4, "profile +0x3C bit 4"},
    {16, A1R_FLAGS, A1X_MAYBE, 0, "save flag 0x16B"},
    {17, A1R_ACTOR, A1X_YES, A1S_C74, "profile +0x74 >= target (cap 100)"},
    {18, A1R_ACTOR, A1X_NO, A1S_B3C_6, "profile +0x3C bit 6"},
    {19, A1R_ACTOR, A1X_MAYBE, A1S_B3C_7, "profile +0x3C bit 7"},
    {20, A1R_ACTOR, A1X_NO, A1S_B3C_8, "profile +0x3C bit 8"},
    {21, A1R_ACTOR, A1X_MAYBE, A1S_B44_0, "profile +0x44 bit 0 clear"},
    {22, A1R_FLAGS, A1X_MAYBE, 0, "save flag 0x16A"},
    {23, A1R_ACTOR, A1X_MAYBE, A1S_C70, "profile +0x70 >= target (cap 100)"},
    {24, A1R_ACTOR, A1X_MAYBE, A1S_C71, "profile +0x71 >= target (cap 100)"},
    {25, A1R_NA, A1X_NO, 0, "Raid"}, {26, A1R_NA, A1X_NO, 0, "Raid"}, {27, A1R_NA, A1X_NO, 0, "Raid"}, {28, A1R_NA, A1X_NO, 0, "Raid"}, {29, A1R_NA, A1X_NO, 0, "Raid"},
    {30, A1R_NA, A1X_NO, 0, "Raid"}, {31, A1R_NA, A1X_NO, 0, "Raid"}, {32, A1R_NA, A1X_NO, 0, "Raid"}, {33, A1R_NA, A1X_NO, 0, "Raid"}, {34, A1R_NA, A1X_NO, 0, "Raid"},
    {35, A1R_NA, A1X_NO, 0, "Raid"}, {36, A1R_NA, A1X_NO, 0, "Raid"}, {37, A1R_NA, A1X_NO, 0, "Raid"}, {38, A1R_NA, A1X_NO, 0, "Raid"}, {39, A1R_NA, A1X_NO, 0, "Raid"},
    {40, A1R_NA, A1X_NO, 0, "Raid"}, {41, A1R_NA, A1X_NO, 0, "Raid"}, {42, A1R_NA, A1X_NO, 0, "Raid"}, {43, A1R_NA, A1X_NO, 0, "Raid"}, {44, A1R_NA, A1X_NO, 0, "Raid"},
    {45, A1R_NA, A1X_NO, 0, "Raid"}, {46, A1R_NA, A1X_NO, 0, "Raid"}, {47, A1R_NA, A1X_NO, 0, "Raid"}, {48, A1R_NA, A1X_NO, 0, "Raid"}, {49, A1R_NA, A1X_NO, 0, "Raid"},
    {50, A1R_NA, A1X_NO, 0, "Raid"},
};

static const ach1_entry_t *ach1_by_id(unsigned id)
{
    return id >= 1 && id <= ACH1_N ? &ach1_table[id - 1] : 0;
}

/* ---- the profile setters (the places the gate sits in front of) ------------------------------------------------------------------------------------------------------------- */
enum {                       /* whose action is it */
    A1B_ACTOR = 1,           /* a human unit's action (the unit's slot owner decides) */
    A1B_KILLER               /* the enemy's death: the killer decides; deaths without a human killer count as in the single-player game */
};

typedef struct {
    unsigned char sid;
    unsigned char bound;
    unsigned char cap;       /* counter cap in the game (0 = a bit / a mask) */
    unsigned char rate;      /* credits per second one PC can plausibly make (anything above is refused and logged) */
    unsigned short sect_cap; /* credits per section instance */
    unsigned func;           /* the setter in the exe */
    unsigned caller;         /* its game-side call site (the hook candidates) */
    const char *what;
} ach1_setter_t;

static const ach1_setter_t ach1_setters[A1S_N] = {
    {A1S_NONE, 0, 0, 0, 0, 0, 0, ""},
    {A1S_C70, A1B_ACTOR, 100, 3, 120, 0x499260, 0x685E60, "ladder climb (ClimbLadderState vf41/53/65 -> 0x685E60)"},
    {A1S_C71, A1B_ACTOR, 100, 8, 150, 0x499320, 0x6871B0, "charge-action tech 0x6871B0/0x6873B0, explode kind 8/9 0x5FD0B0"},
    {A1S_C72, A1B_KILLER, 150, 16, 400, 0x4993E0, 0x899CA0, "enemy death rank hook 0x899CA0 <- uEnemyBase_vf43 0x5798F0"},
    {A1S_C74, A1B_ACTOR, 100, 8, 150, 0x499530, 0x5B8530, "zombie hit 0x5B8530 <- uZombieBase_vf43"},
    {A1S_SCAN, A1B_ACTOR, 0, 4, 60, 0x4996D0, 0x8BA5A0, "Genesis scan of an enemy kind 0x8BA5A0 <- 0x7A3EC0"},
    {A1S_B3C_4, A1B_ACTOR, 0, 8, 60, 0x4997B0, 0x605090, "uWpBoneArrow_vf120"},
    {A1S_B3C_6, A1B_KILLER, 0, 8, 60, 0x4998D0, 0x53E0F0, "cScagdeadStateBase_vf151"},
    {A1S_B3C_7, A1B_ACTOR, 0, 8, 60, 0x499960, 0x5FCFD0, "uWeaponExplode effect, 3 or more targets"},
    {A1S_B3C_8, A1B_KILLER, 0, 8, 60, 0x4999F0, 0x557850, "uScarmiglione_vf43 / 0x558290"},
    {A1S_B44_0, A1B_ACTOR, 0, 8, 60, 0x499200, 0x6B95E0, "upper-body state vf183"},
};

/* ---- who did it -------------------------------------------------------------------------------------------------------------------------------------------------------------- */
enum {
    A1A_NONE = 0,            /* no human: environment, a trap, an unknown killer */
    A1A_LOCAL,               /* a human of THIS PC (own slot; control kind 0 or 1 driven by this PC's pad) */
    A1A_REMOTE,              /* the partner's human on this PC: the puppet (control kind 3), or the client's attacker seen by the host */
    A1A_AI                   /* the game's AI partner (control kind 2) */
};

enum {                       /* gate result */
    A1G_ALLOW = 0,           /* the game's own setter call stands */
    A1G_UNDO_REMOTE,         /* it is the partner's action: skip the setter (the partner's PC credits it) */
    A1G_UNDO_NOACTOR,        /* an actor-bound setter without a local human */
    A1G_UNDO_DUP,            /* the same cause was already credited (double run: the local logic and a replay) */
    A1G_UNDO_RATE,           /* more credits per second than a human can make */
    A1G_UNDO_SECT,           /* section cap reached */
    A1G_UNDO_TAINT           /* the section was touched by a test / warp command: no credit */
};

typedef struct {
    int session;             /* a co-op session is running (otherwise the game is untouched) */
    int gate_on;             /* default 1 (a lab switch could turn it off) */
    int tainted;             /* a test / warp / teleport command was used in this section */
} ach1_cfg_t;

#define A1_LEDGER 256
typedef struct {
    unsigned long long key[A1_LEDGER];   /* (setter << 56) | key; 0 = empty. Open addressing, cleared at every section instance */
    unsigned win_start[A1S_N], win_n[A1S_N];
    unsigned sect_n[A1S_N];
    unsigned refused[7];                 /* counts per A1G_UNDO_* result, for the log line */
} ach1_state_t;

static void ach1_state_init(ach1_state_t *s) { memset(s, 0, sizeof *s); }

/* a new section instance (area load, checkpoint retry): the dedupe ledger and the section counts start again */
static void ach1_new_section(ach1_state_t *s)
{
    memset(s->key, 0, sizeof s->key);
    memset(s->win_start, 0, sizeof s->win_start);
    memset(s->win_n, 0, sizeof s->win_n);
    memset(s->sect_n, 0, sizeof s->sect_n);
}

static int ach1_ledger_has(const ach1_state_t *s, unsigned long long k)
{
    unsigned h = (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 56);
    for (unsigned i = 0; i < A1_LEDGER; i++) {
        unsigned long long v = s->key[(h + i) % A1_LEDGER];
        if (!v) return 0;
        if (v == k) return 1;
    }
    return 0;
}

static int ach1_ledger_add(ach1_state_t *s, unsigned long long k)
{
    unsigned h = (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 56);
    for (unsigned i = 0; i < A1_LEDGER; i++) {
        unsigned long long *v = &s->key[(h + i) % A1_LEDGER];
        if (*v == k) return 1;
        if (!*v) { *v = k; return 1; }
    }
    return 0;   /* full: the section cap (<= 400 per setter) is the real limit; a full ledger refuses nothing */
}

/*
 * The gate: the game is about to run setter `sid` (a call in front of the profile write) because of `actor`; `key` identifies the cause (enemy placement id, scanned kind;
 * 0 = no dedupe, ladders and tech actions repeat legitimately); `now_ms` is the sender-independent local clock. Returns A1G_ALLOW to let the game's call run.
 * Only an ALLOW changes the ledger / the windows, so a refused call leaves no trace.
 *
 *  - session off or gate off: the game is untouched.
 *  - tainted section (a test command was used): nothing is credited (an unlock on a profile whose section was warped to is not a clean unlock).
 *  - A1B_ACTOR setters: LOCAL credits; REMOTE (puppet) refuses (the partner's PC credits it); AI / NONE refuse (the puppet and the AI never act for this profile).
 *  - A1B_KILLER setters: LOCAL credits; REMOTE refuses; NONE and AI credit as the single-player game does (a trap kill, an AI partner kill), because the partner's PC runs the
 *    same death and credits its own profile too: both profiles are separate, nothing is credited twice to one profile.
 *  - dedupe (key != 0): the same (setter, key) in one section instance credits once.
 *  - rate and section caps: a refusal is a counter in the state, never an error.
 */
static int ach1_gate(const ach1_cfg_t *c, ach1_state_t *s, unsigned sid, int actor, unsigned key, unsigned now_ms)
{
    const ach1_setter_t *st;
    unsigned long long k = 0;
    int r = A1G_ALLOW;
    if (!c->session || !c->gate_on) return A1G_ALLOW;
    if (sid <= A1S_NONE || sid >= A1S_N) return A1G_ALLOW;
    st = &ach1_setters[sid];
    if (c->tainted) r = A1G_UNDO_TAINT;
    else if (actor == A1A_REMOTE) r = A1G_UNDO_REMOTE;
    else if (st->bound == A1B_ACTOR && actor != A1A_LOCAL) r = A1G_UNDO_NOACTOR;
    else if (st->bound == A1B_KILLER && actor != A1A_LOCAL && actor != A1A_NONE && actor != A1A_AI) r = A1G_UNDO_NOACTOR;
    if (r == A1G_ALLOW && key) {
        k = ((unsigned long long)sid << 56) | key;
        if (ach1_ledger_has(s, k)) r = A1G_UNDO_DUP;
    }
    if (r == A1G_ALLOW && s->sect_n[sid] >= st->sect_cap) r = A1G_UNDO_SECT;
    if (r == A1G_ALLOW) {
        if ((unsigned)(now_ms - s->win_start[sid]) >= 1000 || !s->win_n[sid]) s->win_start[sid] = now_ms, s->win_n[sid] = 0;
        if (s->win_n[sid] >= st->rate) r = A1G_UNDO_RATE;
    }
    if (r != A1G_ALLOW) { s->refused[r]++; return r; }
    s->win_n[sid]++;
    s->sect_n[sid]++;
    if (k) ach1_ledger_add(s, k);
    return A1G_ALLOW;
}

/* a peer may only report things about its own human: a message that claims an action of another slot is dropped (the partner never decides what this profile earns) */
static int ach1_claim_ok(int sender_slot, int claimed_actor_slot)
{
    return sender_slot >= 0 && sender_slot == claimed_actor_slot;
}

/* The episode clear (ids 1-8): the PC records the clear of the section it played at the session difficulty. A client whose own menu difficulty differs does not record a clear
 * under another difficulty than the one it played (the lobby carries the host's difficulty: local == session is the normal case). */
static int ach1_clear_ok(const ach1_cfg_t *c, int local_diff, int session_diff, int reached_end)
{
    if (!c->session || !c->gate_on) return 1;
    if (c->tainted || !reached_end) return 0;
    return local_diff == session_diff;
}

/* Are the Steam writers blocked? REV1_ACHBLOCK=1 blocks, =0 passes, unset follows the test console (REV1_CONSOLE set = a lab / test run = blocked; a player's start = real). */
static int ach1_block_default(int has_env, char env_first, int has_console)
{
    return has_env ? env_first == '1' : has_console;
}

/* ---- the game's own condition, as a pure function over a copy of the local state (for the offline model and for logging "would unlock" before the game polls) ------------------ */
typedef struct {
    unsigned char c70, c71, c72, c73, c74;   /* profile counters */
    unsigned b3c, b44, scan6c;               /* profile +0x3C, +0x44, +0x6C */
    unsigned flags30;                        /* save flags 0x120..0x13D, bit i = flag 0x120 + i */
    unsigned char f16a, f16b;                /* save flags 0x16A, 0x16B */
    unsigned char ep_clear[12][3];           /* Episode record n cleared on difficulty d (0x899490) */
    unsigned deaths_counter;                 /* 0x89D490 result */
    int ep_system;                           /* [0x116A7E4] != 0 */
    int coop_flag;                           /* [sCoopManager+0x28] & 1: only the Raid modes set it */
    unsigned short target[ACH1_N + 1];       /* info+0x20 per id (loaded from game data; 0 = not known, such an id never reports done) */
    unsigned sent;                           /* "sent" bits of ids 1..24 (bit id-1) */
    unsigned active;                         /* cAchievementInfo state == 1 (bit id-1): the game only polls active infos; id 21 is true on a fresh profile, so it matters */
} ach1_profile_t;

static int ach1_pop(unsigned v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static int ach1_check(const ach1_profile_t *p, unsigned id)
{
    int i, j, grp, ok, n;
    if (id < 1 || id > ACH1_N) return 0;
    if (id >= 25 && !p->coop_flag) return 0;
    if (id >= 25) return 0;   /* Raid conditions are never evaluated by the mod */
    switch (id) {
    case 1: case 2: case 3: case 4:
        grp = (int)(id - 1) * 3;
        for (j = 0, ok = 1; j < 3; j++) {
            int any = p->ep_clear[grp + j][0] || p->ep_clear[grp + j][1] || p->ep_clear[grp + j][2];
            if (!any) ok = 0;
        }
        return ok;
    case 5: return (p->b3c & 1) || (p->b3c & 2) || (p->b3c & 4);
    case 6: return (p->b3c & 2) || (p->b3c & 4);
    case 7: return (p->b3c & 4) != 0;
    case 8:
        if (!p->ep_system) return 0;
        return p->deaths_counter == 0 && ((p->b3c & 2) || (p->b3c & 4));
    case 9: case 10: case 11: n = ach1_pop(p->flags30 & 0x3FFFFFFFu); break;
    case 12: n = ach1_pop(p->scan6c & 0x1FFFFu); break;
    case 13: return ach1_pop(p->scan6c & 0x1FFFFu) > 16;
    case 14: n = p->c72; break;
    case 15: return (p->b3c >> 4) & 1;
    case 16: return p->f16b != 0;
    case 17: n = p->c74; break;
    case 18: return (p->b3c >> 6) & 1;
    case 19: return (p->b3c >> 7) & 1;
    case 20: return (p->b3c >> 8) & 1;
    case 21: return (p->b44 & 1) == 0;
    case 22: return p->f16a != 0;
    case 23: n = p->c70; break;
    case 24: n = p->c71; break;
    default: return 0;
    }
    i = p->target[id];
    return i > 0 && n >= i;
}

/* the setters' writes, as the game does them (caps included); `arg` = scan kind 0..16 for A1S_SCAN */
static void ach1_apply(ach1_profile_t *p, unsigned sid, unsigned arg)
{
    switch (sid) {
    case A1S_C70: if (p->c70 < 100) p->c70++; break;
    case A1S_C71: if (p->c71 < 100) p->c71++; break;
    case A1S_C72: if (p->c72 < 150) p->c72++; break;
    case A1S_C74: if (p->c74 < 100) p->c74++; break;
    case A1S_SCAN: if (arg < 17) p->scan6c |= 1u << arg; break;
    case A1S_B3C_4: p->b3c |= 0x10; break;
    case A1S_B3C_6: p->b3c |= 0x40; break;
    case A1S_B3C_7: p->b3c |= 0x80; break;
    case A1S_B3C_8: p->b3c |= 0x100; break;
    case A1S_B44_0: p->b44 |= 1; break;
    default: break;
    }
}

/* the polling pass of the game: active ids 1..24 not yet sent whose condition is true become sent; returns the mask of the newly unlocked ids (bit id-1) */
static unsigned ach1_poll(ach1_profile_t *p)
{
    unsigned now = 0;
    for (unsigned id = 1; id <= 24; id++)
        if ((p->active & (1u << (id - 1))) && !(p->sent & (1u << (id - 1))) && ach1_check(p, id)) now |= 1u << (id - 1);
    p->sent |= now;
    return now;
}

/* ---- the glue (native/ach1_game.c): the call sites of the setters that it gates, build 13298487 ------------------------------------------------------------------------------- */
enum { A1C_ESI = 0, A1C_EDI, A1C_HIT_EDI, A1C_HIT_STACK };   /* where the actor is at the call: a unit in a register, or a HitInfo (attacker +0x40, owner +0x44) */
typedef struct {
    unsigned site;           /* the call instruction E8 rel32, preceded by `mov ecx, [0x116B3A4]` (8B 0D A4 B3 16 01) */
    unsigned setter;         /* its target */
    unsigned char sid;
    unsigned char src;
    unsigned char off;       /* the profile counter the setter increments and returns (AL) */
    const char *what;
} ach1_site_t;
static const ach1_site_t ach1_sites[] = {
    {0x685F3C, 0x499260, A1S_C70, A1C_ESI, 0x70, "ladder (ClimbLadder 0x685E60)"},
    {0x687294, 0x499320, A1S_C71, A1C_EDI, 0x71, "charge tech (MoveActionParam_vf17 0x6871B0)"},
    {0x687418, 0x499320, A1S_C71, A1C_ESI, 0x71, "charge tech (MoveActionParam_vf18 0x6873B0)"},
    {0x5FD274, 0x499320, A1S_C71, A1C_ESI, 0x71, "explode kind 8/9 (uWeaponExplode_vf39 0x5FD0B0)"},
    {0x899D01, 0x4993E0, A1S_C72, A1C_HIT_STACK, 0x72, "enemy killed (rank hook 0x899CA0)"},
    {0x5B87C9, 0x499530, A1S_C74, A1C_HIT_EDI, 0x74, "zombie hit (0x5B8530)"},
};
#define ACH1_NSITES ((int)(sizeof ach1_sites / sizeof ach1_sites[0]))

/* the actor of a gated call: a player-class unit's slot (+0xE2C) and kind (+0xE30: 1 human, 2 the game's AI, 3 remote) against this PC's own slot.
 * slot < 0: no player acted (a trap, an enemy, nobody). The own slot is always LOCAL; another slot is the partner's human (his puppet here, or the host's
 * enemy hit by his PCHIT) unless the game drives it as its AI partner (kind 2: he left, single-player rules again). */
static int ach1_actor_class(int slot, int kind, int own)
{
    if (slot < 0) return A1A_NONE;
    if (slot == own) return A1A_LOCAL;
    return kind == 2 ? A1A_AI : A1A_REMOTE;
}
static const char *ach1_actor_name(int a)
{
    return a == A1A_LOCAL ? "this PC's human" : a == A1A_REMOTE ? "the partner's human" : a == A1A_AI ? "the AI partner" : "no human";
}
static const char *ach1_result_name(int r)
{
    static const char *const n[] = {"credited", "skipped (the partner's PC credits it)", "skipped (no human of this PC)", "skipped (already credited)",
                                    "skipped (rate limit)", "skipped (section limit)", "skipped (a lab command touched this section)"};
    return r >= 0 && r <= A1G_UNDO_TAINT ? n[r] : "?";
}

#endif
