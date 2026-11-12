/*
 * rolebarrier1.h - the pure part of B3 / B8 / B9 of the Revelations 1 network game (build 13298487): the role of the joined player, the load and cutscene barrier, the
 * Retry / Continue triple. No Windows API, no game memory: the engine glue (rolebarrier1_game.c) supplies memory and the session through small tables, the offline tests
 * (tests/test_rolebarrier1.c, test_rolebarrier1_x86.c) supply fakes. Background: notes\net-story-re.md (barrier, Retry / Continue), notes\net-role-barrier.md (this work).
 *
 *  1. ROLE      the session gives each human a role (lead = unit slot 0, partner = unit slot 1). Before the episode loads this PC must know its own slot: the partner's
 *               machine answers the engine's own-slot query with 1 (capture_set_own) and plays unit slot 1 on pad 0 (capture_bind), the guards (guards1: the pickup
 *               stub compares with the own slot) read the same value. rb_role_step turns (session started, role) into APPLY / RELEASE once, again when the role changes.
 *  2. HOOKS     five detours of the engine, each with the exact bytes it expects (anything else = refusal with the reason, nothing written). Table rbk_defs:
 *                 epend    cEpisodeEndState_vf7  0x42D160  PRE gate  early return (ret 4)  until the other PC reached the same section end
 *                 pcs      FUN_008916E0 (FsmStartPcsDemo)  PRE observer   a scripted cutscene starts here (count, name)
 *                 movie    FUN_00785580 (FsmPlayDemoMovie) PRE observer   a movie starts here (count, name)
 *                 pcsend   sPcsManager.mPcsIsEnd getter 0x88FC20  POST filter  "ended" is reported to the script only when the other PC ended it too
 *                 restart  FUN_00720870 (Retry / Continue)  PRE rewrite  the host's triple {area, checkpoint A, B} replaces the client's own
 *                 start    FUN_00720540 (the start record from the table)  PRE observer  (lobby-must) the host's story start from the title (New Game,
 *                          Continue / Load of a save outside 1-1 / 1-3) and the checkpoint reload: the host sends the record's triple; a section change
 *                          (cEpisodeEndState, both PCs advance themselves) is told apart by the caller's address
 *  3. BARRIER   two kinds of rendezvous, symmetric ("both wait for each other"), carried by reliable CP_EVENTs of kind CP_EV_RB (ARRIVE{kind, key, n}): the end of a section
 *               and the end of a cutscene. A held PC is released when the other one has arrived at the same point, or by its own timeout (never a deadlock). The LEVEL LOAD
 *               barrier is the session's own LOAD_BEGIN / GO (rb_load_step: begin when this PC's level disappears, done when it is playable again).
 *  4. RESTART   the host sends the triple of every Retry / Continue it performs (RESTART event); the client rewrites its own Continue call with it, so a client never
 *               restarts from its own save.
 */
#ifndef ROLEBARRIER1_H
#define ROLEBARRIER1_H

#include <stddef.h>
#include <stdint.h>

#include "secids1.h"

typedef struct { uint32_t area; int32_t a, b; } rb_triple;   /* a restart: area id, checkpoint ids A / B (section 4) */

/* ---------------------------------------------------------------- 1. role */
enum { RBR_NONE = 0, RBR_APPLY, RBR_RELEASE };
typedef struct {
    int applied;                 /* a role is in force */
    int role;                    /* the role in force: 0 lead, 1 partner */
    unsigned applies, releases;
} rb_role_t;
typedef struct {
    int act;                     /* RBR_* */
    int role;                    /* APPLY: the role to apply */
    int own_req;                 /* APPLY: the argument for capture_set_own: -1 = engine default (lead, as the split lobby does), 1 = unit slot 1 (partner) */
    int prev;                    /* APPLY: the role that was in force before (-1 none): rev1 live 16, the roles of a co-op save put back at the host's title Continue
                                  * change the role in a started session; the caller takes the old role's capture back first */
} rb_role_act;
void rb_role_init(rb_role_t *r);
/* started: the session START came (roles final); role: 0 lead / 1 partner / -1 unknown. Call once per frame; the action is for the caller to carry out. */
int rb_role_step(rb_role_t *r, int started, int role, rb_role_act *out);
int rb_role_own_req(int role);   /* -1 for the lead, 1 for the partner, -1 for anything else */

/* ---------------------------------------------------------------- 2. hooks */
typedef struct {
    int (*read)(void *user, uint32_t addr, void *out, uint32_t n);          /* 1 = all bytes read */
    int (*write)(void *user, uint32_t addr, const void *p, uint32_t n);     /* 1 = all bytes written (the glue lifts the page protection) */
    uint32_t (*alloc)(void *user, uint32_t n);                              /* an executable, writable block that lives for the whole process; 0 = failed */
    void (*log)(void *user, const char *line);                              /* may be NULL */
    void *user;
} rbk_mem;

/* continue1: RBK_LSTART = the aCut area's load step FUN_0041cfc0 (POST filter: the level-start hold of a restart); RBK_CONT = the stock Continue request FUN_0041efe0
 * (PRE gate: the guest's own Continue is told to the host; a second request while one is on its way is dropped) */
/* allep1 (hunk1.h): RBK_BAGID = sItemBox_vf17 0x84DC50, the bag of a character id: PRE observer that rewrites the class argument of a Hunk human to Parker's (the item box has a bag per campaign
 * character, none for Hunk: the engine asserted `pMyBag is invalid pointer` at the level start of a Continue, allep hk1) */
enum { RBK_EPEND = 0, RBK_PCS, RBK_MOVIE, RBK_PCSEND, RBK_RESTART, RBK_START, RBK_LSTART, RBK_CONT, RBK_BAGID, RBK_PCLSC, RBK_PCLSP, RBK_N };
/* allep1 (hunk1.h): RBK_PCLSC = FUN_0042A730 (creates the section's humans from the class ids [[0x116A340] + 0x24 / 0x28]) and RBK_PCLSP = FUN_0042B720 (preloads their resources): PRE observers that
 * write the Hunk class into slot 1 right before the engine reads it (same thread: no race with the engine's own write; the 3 ms persist thread lost that race on the host PC in allep hk1) */
enum { RBK_PRE = 0, RBK_POST };
#define RBK_SIG_MAX 12

typedef struct {
    const char *name;            /* console / log name */
    const char *what;            /* the engine function */
    uint32_t va;
    uint8_t sig[RBK_SIG_MAX];    /* the bytes that must be there (whole instructions, position independent) */
    int steal;                   /* number of bytes taken over by the detour (>= 5), = bytes of sig that are checked */
    int kind;                    /* RBK_PRE: callback(frame) before the function, may return early; RBK_POST: callback(this, result) after the function */
    int hold_ret;                /* PRE: the immediate of the `ret` of an early return (0 = plain ret, 4 = ret 4); -1 = the function can never be skipped (observer) */
} rbk_def;
extern const rbk_def rbk_defs[RBK_N];

/* POST stub: the callback gets `this` and the FULL eax of the function and returns the eax to hand on (bit for bit the same when it does not filter); ecx and edx are kept
 * as the function left them. */

/* what a PRE callback sees: the registers of the call (pushad order) and the stack as the function would see it. Arguments can be changed in place (the restart hook does). */
typedef struct {
    uint32_t edi, esi, ebp, esp_pushad, ebx, edx, ecx, eax;
    uint32_t eflags, hold_cell;
    uint32_t ret_addr;           /* the caller's return address */
    uint32_t arg[8];             /* the stack arguments: arg[0] is the first one */
} rbk_frame;

enum { RBK_OFF = 0, RBK_ON, RBK_MISMATCH, RBK_ERROR };
typedef struct {
    int st;
    uint32_t block;              /* the stub block (0 = none yet) */
    uint32_t cb;                 /* the callback it calls */
    char why[160];               /* reason of the last refusal */
    unsigned applies, reverts, refusals;
    volatile unsigned calls;     /* bumped by the glue's callback */
    volatile unsigned changed;   /* calls where the hook changed what the engine does (held, rewritten, a result replaced); observers: always 0 */
    volatile unsigned episodes;  /* separate stretches of changed calls (a hold that lasts 100 frames = 1) */
} rbk_state;

/* the stub for a definition, placed at stub_addr (PRE: needs `cell` unused; POST: a 4-byte cell at `cell`); returns the length, 0 when cap is too small. Layout in the .c file. */
int rbk_stub_build(uint8_t *out, size_t cap, const rbk_def *d, uint32_t stub_addr, uint32_t cb, uint32_t cell);
void rbk_jmp_build(uint8_t out[5], uint32_t from, uint32_t to);
/* apply: check the bytes, build the stub, write the jump. 1 = in force (also when it already was), 0 = refused (s->why). Safe to call every frame. */
int rbk_apply(rbk_state *s, const rbk_def *d, const rbk_mem *m, uint32_t callback);
int rbk_revert(rbk_state *s, const rbk_def *d, const rbk_mem *m);
/* the live bytes in words: "original" / "detoured (ours)" / "FOREIGN xx xx ..." / "unreadable" */
int rbk_probe(const rbk_def *d, const rbk_mem *m, uint32_t stub, char *out, size_t size);
const char *rbk_state_name(int st);

/* ---------------------------------------------------------------- 3. barrier */
#define CP_EV_RB_KIND 0x141u     /* = CP_EV_USER + 0x41 (coop_proto.h): the numbers 0x140 (world) and 0x141 (this) are ours in this tree */
/* RB_LSTART (continue1): the level start after a restart (the aCut area's load step FUN_0041cfc0 answered "ready"): both PCs start the reloaded level together */
enum { RB_SECTION = 0, RB_PCS = 1, RB_LSTART = 2, RB_PT_N = 3 };
enum { RB_PROCEED = 0, RB_HOLD = 1 };
/* RBM_SECTION (ep1-flow E4): host -> client, the host passed a section end (any episode): the next section's start triple {seq, area, a, b, from, how};
 * RBM_EPDONE (ep1-flow E5): either PC -> the other, this PC passed the end of co-op (its section change went to the end of the campaign, the solo 16-2 or a non-section) {from};
 * RBM_CONTREQ (continue1): client -> host, the guest chose Continue (its stock Continue request FUN_0041efe0) and no host triple was waiting {seq} */
enum { RBM_ARRIVE = 1, RBM_RESTART = 2, RBM_SECTION = 3, RBM_EPDONE = 4, RBM_CONTREQ = 5 };   /* 6 = RBM_CPPASS (section 7) */
#define RB_SECTION_MSG_LEN 21
#define RB_EPDONE_MSG_LEN 6
#define RB_CONTREQ_MSG_LEN 4
/* continue1: RBM_RESTART carries the host's campaign checkpoint record (sCheckPoint [0x116A334] + 0x60 + mode * 0x24): byte 16 the difficulty (0xFF = not known),
 * 17..18 the mID of the last checkpoint passed (record +0x18, s16), 19..20 the mask of the passed mIDs (record +0x10, u16). Older lengths 16 / 17 stay valid. */
#define RB_RESTART_CP_LEN 21
/* rev1 live 15: RBM_RESTART with the START KIND (22 bytes): bytes 0..20 as the checkpoint form (byte 16 the difficulty or 0xFF; 17..20 the record, valid when bit 2
 * of byte 21 is set), byte 21 bits 0..1 = RB_KIND_*. The guest reads its own save at the title before it follows only for a SAVE start (the host's title Continue,
 * its checkpoint reload, its game-over Continue): a New Game must keep the New Game bag. The older lengths 16 / 17 / 21 stay valid (kind unknown). */
#define RB_RESTART_K_LEN 22
enum { RB_KIND_UNKNOWN = 0, RB_KIND_NEW = 1, RB_KIND_SAVE = 2 };
#define RB_SCP_FN_LO 0x6D0000u      /* the sCheckPoint module (FUN_006D0CB0 / FUN_006D0FF0): FUN_00720540 from here = a save's start (title Continue / Load) */
#define RB_SCP_FN_HI 0x6D2000u
#define RB_TITLE_FN_LO 0x82A000u    /* the title menu manager (FUN_0083CC60 ...): FUN_00720540 from here = New Game (0x83BEF9 / 0x83C0CC / 0x83E896) */
#define RB_TITLE_FN_HI 0x845000u
#define RB_MSG_VER 1
#define RB_MSG_MAX 24

typedef struct {
    int started;                 /* the session has started and is still on */
    int phase;                   /* enum cp_phase (1 lobby, 2 loading, 3 playing) */
    unsigned epoch;              /* level instance counter */
    int gos;                     /* GO notifications seen since the start */
    uint32_t other_area;         /* rev1 live 8: the area the other PC named in its last LOAD_BEGIN (the level it loads; 0 = not known: an older mod, a load of unknown cause) */
    unsigned other_area_epoch;   /* ... the epoch of that LOAD_BEGIN */
} rb_sinfo;

typedef struct {
    uint32_t (*now)(void *user);                         /* milliseconds, monotonic */
    int (*session)(void *user, rb_sinfo *out);            /* 1 = a started network session; fills out */
    int (*send)(void *user, const void *body, unsigned len);    /* a reliable event to the other player: 1 = sent */
    void (*load_begin)(void *user, uint32_t key);         /* the session's LOAD_BEGIN (lk_load_begin) */
    void (*load_done)(void *user);                        /* LOAD_DONE (lk_load_done) */
    void (*log)(void *user, const char *line);            /* may be NULL */
    void *user;
} rb_io;

typedef struct {
    uint32_t section_ms;         /* a held section end gives up after this long (default 180000) */
    uint32_t pcs_ms;             /* a held cutscene end (default 60000) */
    uint32_t latch_ms;           /* a gate that was not asked for this long starts afresh (default 3000; never for a pending cutscene end, see pcs_relatch) */
    int pcs_relatch;             /* LAB / the model before the fix (ep1-probe e1p1): 1 = a held cutscene end not asked for latch_ms starts afresh as a new occurrence
                                    (e1p1 live: the host's mPcsIsEnd said "ended" only every 8.3 s, every read was a new occurrence #2..#31 that the late partner's
                                    arrival #1 never matched and whose timeout restarted: the host's FSM waited for ever). Default 0: a pending cutscene keeps
                                    its occurrence until the partner's arrival or the timeout; rb_pcs_begin starts the next one. */
    uint32_t settle_ms;          /* the level must be playable this long before LOAD_DONE (default 500) */
    uint32_t unplay_ms;          /* ... and gone this long before LOAD_BEGIN (default 700) */
    uint32_t stale_epoch_ms;     /* an epoch the other PC started while this level stays: report loaded anyway after this long (default 8000) */
    int no_triple_hold_ms;       /* client Continue without a host triple: 0 = go on with its own (default), >0 = answer "not now" for this long */
    int follow_ms;               /* client: a host triple that no own Continue used within this time makes the client restart by itself with it (0 = never; default 1500) */
    /* ep1-flow E4: the host's next section (RBM_SECTION) on the client */
    int sec_follow_ms;           /* a client that is not at the end of the section itself follows the host into the next section after this long (default 4000) */
    int sec_window_ms;           /* the client's own section change to the same area this close to the host's triple is the same change: no follow (default 20000) */
    int sec_wait_ms;             /* a client held at its own section end waits this long for its own change before it follows anyway (default 30000) */
    /* ep1-flow E5: the end of Episode 1 */
    uint32_t ep_close_cap_ms;    /* after this PC's own end of Episode 1 the session closes at the latest after this long (default 1800000) */
    /* rev1 live 10: 1 (default) = a PC still in the section the host left follows the host's next section by its OWN stock section end (RB_FOL_SECEND: the
     * glue sets the section-end game flags the section's final script sets; cEpisodeEndState_vf7 then changes the section as in a single-player game: the
     * start table's triple, the checkpoint slot cleared, the result screen, the carried bag); 0 = the model before the fix (FUN_00720870 from the running
     * level: l8c / l9b / l9d, the followed 1-3 had no scene scripts). The title route of r14 / r18 (fd157ce) is gone: it did not load the scenes either. */
    int sec_stock;
    /* rev1 live 8 (ef4 crash exe+9d344): the cross-level guard. 1 (default) = the stale fallback ("reporting loaded anyway") is refused while the other PC's
     * LOAD_BEGIN named another section than this PC's level: the two PCs never share a PLAYING epoch with different levels; 0 = the model before the fix */
    int xl_guard;
    /* continue1 (e1p6: the guest's follow loaded 11_010_0 for 112 s while the host's reloaded level was live; the Ooze killed the host again) */
    int cp_sync;                 /* 1 (default): the host's checkpoint record travels with RESTART and the guest's record takes it before its reload; 0 = the model before the fix */
    int lstart_hold;             /* 1 (default): after a restart each PC's level start waits for the other PC's (RB_LSTART); 0 = the model before the fix (the first PC plays alone) */
    uint32_t lstart_ms;          /* ... at most this long (default 120000) */
    int shield;                  /* 1 (default): from a restart until the session's barrier opens the own humans take no hit (the glue's ApplyHit / PCHIT); 0 = before the fix */
    uint32_t shield_ms;          /* ... at most this long (default 300000, the session barrier's own limit) */
    int follow_stock;            /* 1 (default): the guest's follow in a level goes through its own stock Continue (FUN_0041efe0); 0 = the direct FUN_00720870 call */
    uint32_t stock_wait_ms;      /* the stock Continue has not used the host's triple this long: the direct call after all (default 8000) */
    int title_stock;             /* rev1 live 14 (m14d, LAB, default 0): at the TITLE too the guest follows through its own stock Continue (FUN_0041efe0 -> aStartContinue ->
                                    sCheckPoint FUN_006D0FF0 reads its save = its bag) when its save holds the host's checkpoint; 0 = the direct FUN_00720870 call at the
                                    title (m14d: the guest's own human had NO weapon after the host's title Continue) */
    int cont_req;                /* 1 (default): the guest's own Continue without a host triple asks the host (RBM_CONTREQ) and waits; 0 = before the fix (own checkpoint) */
    uint32_t cont_wait_ms;       /* ... at most this long, then its own checkpoint (default 15000) */
    uint32_t cont_dedup_ms;      /* a second stock Continue request within this time (no level loaded since) is dropped (default 10000) */
    int cp_mirror;               /* task 4: 1 (default) = a checkpoint passed on one PC is passed on the other too (RBM_CPPASS); 0 = the model before the fix */
    /* rev1 live 15 (m14d / m14h / m14j, LIVE: after the host's TITLE Continue the guest's own human had NO weapon): the stock title Continue reads the save
     * first (uTitleMenuManager state 0x22, item Continue: uSaveFlowHD [manager + 0x68] -> FUN_008b3430(slot 0) -> FUN_008b9260(..., 0, 6): sItemBox, the
     * flags, the record), then FUN_006d0ff0 -> FUN_00720870; the guest's follow called FUN_00720870 at once and never read its save, and the level start
     * takes the bag from sItemBox (FUN_005C8CC0 -> sItemBox vf+0x48(the character)). 1 (default): at the title, for a SAVE start of the host, the guest
     * reads its own save the stock way first and calls FUN_00720870 after (rb_title_load_*); 0 = the model before the fix (the direct call at once) */
    int title_load;
    uint32_t title_load_ms;      /* ... the read has not finished this long: the direct call after all (default 15000) */
} rb_cfg;

/* continue1: the campaign checkpoint record of sCheckPoint ([0x116A334] + 0x60 + mode * 0x24): +0x10 the mask of the passed checkpoint mIDs (1 << mID), +0x18 the mID
 * of the last checkpoint passed (0 / -1 = the section start); FUN_006d1a70 writes it when a CheckPoint zone content is passed (1-1: mID 1 = 256 / 4 flag 4390,
 * mID 2 = 256 / 5) */
typedef struct { int have; int32_t id; uint32_t mask; } rb_cp;
/* the whole record as the checkpoint mirror (section 7) compares it: mID, mask, area, checkpoints A / B */
typedef struct { int have; int32_t id; uint32_t mask, area; int32_t a, b; } rb_cprec;

typedef struct { uint8_t kind; uint32_t key; uint16_t n; int used; } rb_arrival;
typedef struct { uint8_t kind; uint32_t key; uint16_t count; } rb_occ;

typedef struct {
    int st;                      /* 0 idle, 1 waiting, 2 released by the peer, 3 released by the timeout */
    uint32_t key;
    uint16_t n;
    uint32_t t0, last;
    uint32_t held;               /* frames held in this visit */
} rb_point;

typedef struct {
    rb_io io;
    rb_cfg cfg;
    int enabled;                 /* the rendezvous / load logic runs (the glue sets it while a network session is on or REV1_BARRIER=1) */
    rb_point pt[RB_PT_N];
    rb_arrival peer[32];
    int peer_next;
    rb_occ occ[24];
    int occ_next;
    int host;                    /* this PC is the host (a RESTART event is only taken by the client) */
    int pcs_pending;             /* a cutscene began and its end has not been released yet */
    uint32_t pcs_key;
    /* counters for the status line */
    unsigned arrivals_tx, arrivals_rx, released_peer, released_timeout, holds, pcs_begins, movie_begins, section_ends;
    unsigned pcs_ambient;        /* FsmStartPcsDemo of a scene script (game\scene\..., e.g. the wave sound loop): counted, never a rendezvous */
    unsigned k_holds[RB_PT_N], k_episodes[RB_PT_N], k_peer[RB_PT_N], k_timeout[RB_PT_N], k_nosession[RB_PT_N];   /* per kind: held frames, hold stretches, releases, passes without a session */
    unsigned pcs_changed;        /* getter calls whose answer was replaced (always a hold: "not ended" instead of "ended") */
    int nosess_logged[RB_PT_N];
    char why[160];               /* the last decision in words */
    /* load barrier */
    int play_prev;
    uint32_t play_since, unplay_since;
    int seen_playable;           /* playable since the last GO */
    int unplayed_in_epoch;       /* this PC's level went away since the current LOADING epoch started */
    int own_began, epoch_valid, epoch_first;
    int reported_valid;
    unsigned reported_epoch, epoch_seen;
    uint32_t epoch_t0;
    unsigned loads_begun, loads_done, loads_stale;
    /* rev1 live 8: the cross-level guard. lvl_area = the section of this PC's level or of the level it goes to (its own section change, a restart it makes or
     * follows, the host's story start; 0 = not known yet); it goes out with this PC's LOAD_BEGIN */
    uint32_t lvl_area;
    unsigned loads_xl;           /* stale fallbacks refused because the other PC loads another section */
    unsigned xl_epoch;           /* the epoch of the last refusal (+1; 0 = none): logged once per epoch */
    int first_load;              /* no LOADING epoch seen yet in this session */
    /* restart */
    int rs_have;                 /* client: a triple from the host is waiting */
    uint16_t rs_seq, rs_tx_seq;
    uint32_t rs_area;
    int32_t rs_a, rs_b;
    uint32_t rs_t;
    unsigned rs_sent, rs_received, rs_rejected, rs_applied, rs_unmatched;
    uint16_t rs_follow_seq;      /* the seq of the triple the follow was already tried for (once per triple) */
    unsigned rs_followed;
    uint32_t rs_wait_t0;
    /* lobby-must: the host's story start (FUN_00720540 observer) and the last triple sent (one start = one RESTART even when both writers run) */
    int st_pending;
    uint32_t st_ret, st_t;
    unsigned st_calls, st_section, st_sent, st_dup, st_bad;
    int tx_have;
    rb_triple tx_last;
    uint32_t tx_t;
    /* round 2: the host's campaign difficulty travels with its restart (byte 16 of RBM_RESTART); the guest's game takes it before it loads */
    int tx_diff;                 /* host: the difficulty to send with the next restart (0..3, -1 = unknown: not sent); the glue sets it */
    int rs_diff;                 /* client: the difficulty that came with the waiting triple (-1 = none) */
    int applied_diff;            /* client: the difficulty that came with the triple rb_restart_client just used (-1 = none) */
    /* ep1-flow E4: section changes (FUN_00720540 from cEpisodeEndState_vf7) */
    int sec_pending;             /* client: the waiting triple (rs_*) is the host's next section (RBM_SECTION), not a Continue */
    uint32_t sec_from;           /* the section the host left */
    int sec_how;                 /* how the host passed its end: RB_SEC_PEER (this PC had arrived) / RB_SEC_TIMEOUT / RB_SEC_ALONE (no rendezvous ran) */
    int own_sec_have;            /* this PC's own section change: the next area and when */
    uint32_t own_sec_area, own_sec_t, own_sec_from;
    uint16_t sec_tx_seq;
    /* rev1 live 8 (l8a 10:13): the client's dedup of the SECTION follow has its own numbers. SECTION #n (sec_tx_seq) and RESTART #n (rs_tx_seq) are counted
     * apart on the host; stored in one rs_seq, SECTION #1 met the rs_follow_seq 1 of the session start's RESTART #1 follow and was never followed */
    uint16_t sec_rx_seq, sec_follow_seq;
    int sec_follow_valid;
    /* rev1 live 10: the follow by the stock section end. FUN_00720870 (the engine's Retry / Continue) called from a running level of 1-2 (l8c) or from the
     * title after FUN_008B1660 (l9b, l9d) loaded 1-3's geometry and units but no 1-3 scene script (FSM zone tests 396 / 492 against the host's 211029 /
     * 229542, no arrange spawn, `world solo` sites 0/0). Static cause: FUN_00720870 sets the restart bits (game flags [0x113AF8C] 1 and 3) and keeps the
     * checkpoint slot; the new area's opening FUN_0041C8D0 then takes the checkpoint-reload branch (restart bit 1 and slot count > 0), with 1-2's slot.
     * The stock section change (cEpisodeEndState_vf7) clears the slot (FUN_006D0F20) and the new area's cEpisodeProcessState (FUN_0042DE90) resets the
     * flags. sec_stock_* = this PC asked its own stock section end for SECTION #sec_stock_seq at sec_stock_t. */
    int sec_stock_asked;
    uint16_t sec_stock_seq;
    uint32_t sec_stock_t;
    unsigned sec_stock_count, sec_stock_fallback;
    unsigned sec_sent, sec_got, sec_consumed, sec_followed, sec_bad, sec_changes;
    unsigned sec_nohold;    /* ef4 fix: section ends that went on without the other PC (rb_section_gate never holds: cEpisodeEndState_vf7 runs once) */
    /* ep1-flow E5: the end of Episode 1 */
    int ep_local;                /* this PC's own section change went out of Episode 1 (the end of 1-3) */
    int ep_peer;                 /* the other PC told RBM_EPDONE */
    uint32_t ep_local_t, ep_peer_t, ep_from;
    unsigned ep_sent, ep_got;
    int ep_seen_flow, ep_closed;  /* the close rule (rb_ep_close_step): the stock end flow (aEpisodeEnd) was seen; the close was asked for */
    int ep_kind;                  /* allep1: what the end was (RB_EK_*) */
    unsigned st_secbad;           /* allep1: host starts / restarts refused by id (RB_ST_NOTSEC) */
    /* continue1 (A): the checkpoint record with the restart */
    rb_cp tx_cp;                 /* host: the record to send with the next restart (the glue reads it in the restart hook) */
    rb_cp rs_cp;                 /* client: the record that came with the waiting triple */
    rb_cp applied_cp;            /* client: the record that came with the triple rb_restart_client just used (have 0 = none) */
    unsigned cp_sent, cp_got, cp_applied;
    rb_triple rs_last;           /* client: the last host triple a restart used, and when (a late second restart call reuses it instead of its own checkpoint) */
    uint32_t rs_last_t;
    int rs_last_have;
    uint16_t rs_last_seq;
    unsigned rs_reused;
    /* continue1 (B): the level start of a restart and the shield */
    int ls_armed;                /* this PC's next level start meets the other PC's */
    uint32_t ls_key, ls_t;
    unsigned ls_arms, ls_holds, ls_peer, ls_timeout, ls_stale;
    int sh_armed;                /* the own humans are shielded until the barrier opens */
    uint32_t sh_t;
    unsigned sh_epoch0;          /* the session epoch when the restart came (the barrier of a later epoch opens the shield) */
    int sh_loading;              /* the session was seen LOADING since the restart (the guest's restart may come when the host's LOADING epoch has begun) */
    unsigned sh_arms, sh_opened, sh_capped;
    unsigned rs_dups;            /* client: a second restart call right after one with the host's triple, dropped */
    int call_stock;              /* the glue: this restart call comes from the stock Continue (FUN_006d0cb0, return 0x6D0E64): only such a call is dropped / held */
    /* continue1 (C): the follow through the stock Continue */
    int fol_try_stock;           /* the glue: rb_restart_follow_at just said "follow": try the stock Continue first (1) or call FUN_00720870 directly (0) */
    int fol_stock;               /* the stock Continue was asked for this triple ... */
    uint16_t fol_stock_seq;
    uint32_t fol_stock_t;
    int fol_direct_done;
    unsigned fol_stock_asked, fol_stock_fallback;
    /* continue1 (D): Continue chosen on the guest */
    uint32_t cont_t;             /* the last stock Continue request of this PC (0 = none since the last level load) */
    int cont_ours;               /* ... it was the mod's own (the follow / the host's answer to a request) */
    unsigned cont_calls, cont_dropped;
    uint16_t cr_tx_seq;
    unsigned cr_sent, cr_got, cr_done, cr_refused;
    int cr_pending;              /* host: the guest asked for a Continue (rb_cont_request_step acts on it) */
    uint32_t cr_t;
    int held;                    /* client: its own Continue's restart was answered "not now": it waits for the host's triple */
    rb_triple held_own;
    uint32_t held_t;
    int held_fallback;           /* client: the wait is over without a host triple: the next restart call takes its own checkpoint */
    unsigned held_count, held_host, held_own_used;
    /* continue1 task 4: the checkpoint pass mirror (section 7) */
    rb_cprec cpm_last, cpm_mark, cpm_in;
    /* c1a live (18:40): the guest's stock Continue loads ITS OWN save during the load, after the restart hook: its flags (only 4449 at the level start), its
     * record (mID 0) -> the opening movie, 112 s, although the hook had written the host's record. So the stock way is taken only when this PC's SAVE holds
     * the host's checkpoint: cp_saved = the record as the engine last wrote it (a pass, own or mirrored, or a save load; not the mod's cp_take); host_cp = the
     * host's last known checkpoint (its RESTART record, its CPPASS) */
    rb_cprec cp_saved, host_cp;
    unsigned stock_refused, own_cont_redirected;
    uint32_t cpm_in_t, cpm_try_t;
    uint16_t cpm_tx_seq;
    unsigned cpm_sent, cpm_got, cpm_calls, cpm_done, cpm_had, cpm_dropped, cpm_other;
    /* rev1 live 15: the start kind (RB_KIND_*) and the guest's own save read at the title before its follow */
    int tx_kind_once;            /* host: rb_start_triple's kind for the next rb_restart_host (0x100 | kind; 0 = the restart hook: a SAVE start) */
    int tx_kind;                 /* host: the kind of the last restart sent */
    int rs_kind;                 /* client: the kind of the waiting triple (RB_KIND_UNKNOWN for the older message lengths and for a SECTION) */
    int tl_state;                /* client: RB_TLS_* */
    uint32_t tl_t;
    rb_triple tl_fol;            /* the follow's triple, called when the read is over */
    unsigned tl_asked, tl_done, tl_failed, tl_timeout, tl_refused, tl_skipped;
} rb_t;

enum { RB_SEC_ALONE = 0, RB_SEC_PEER = 2, RB_SEC_TIMEOUT = 3 };   /* = rb_point.st of the section end when the host passed it (0 = not held / no rendezvous) */

void rb_defaults(rb_cfg *c);
void rb_init(rb_t *b, const rb_io *io, const rb_cfg *cfg);   /* cfg NULL = defaults */
void rb_reset(rb_t *b);                                       /* a new session / the session ended: forget every rendezvous */
void rb_set_enabled(rb_t *b, int on);

/* the message bodies of CP_EV_RB_KIND */
int rb_msg_arrive(uint8_t *out, size_t cap, int kind, uint32_t key, uint16_t n);                       /* returns the length or 0 */
int rb_msg_restart(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b);
int rb_msg_restart_d(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b, int diff);   /* diff 0..3: 17 bytes, else 16 */
int rb_msg_contreq(uint8_t *out, size_t cap, uint16_t seq);   /* RB_CONTREQ_MSG_LEN or 0 */
#define RB_DIFF_MAX 3
int rb_on_event(rb_t *b, const uint8_t *body, unsigned len);       /* an event from the other PC: 1 = understood and applied, 0 = ignored (reason in b->why for bad ones) */

/* a hook reached a point where the PCs should meet. key: any 32-bit name of the point (a hash of the cutscene name; 0 = unnamed, the n-th unnamed point of the kind). RB_HOLD = come back next frame. */
int rb_gate(rb_t *b, int kind, uint32_t key);
void rb_section_end_seen(rb_t *b);                   /* statistics */
/* The section end's gate (the epend hook). cEpisodeEndState_vf7 runs ONCE per section end (live ef4 2026-11-08: `epend calls 1`), so a hold by its early
 * return drops this PC's section change for good: the host that arrived first stayed in 1-2 while the client went on into 1-3. This gate sends the arrival
 * (the partner's log, the SECTION's how) and ALWAYS returns RB_PROCEED; the next level's load barrier (LOAD_BEGIN / GO) brings the PCs together and a PC that
 * never reaches its own end follows the host's SECTION (E4). */
int rb_section_gate(rb_t *b);
/* FUN_008916E0 ran. Only a real cutscene (a `demo\...` resource, rb_pcs_is_cutscene) arms the rendezvous of its end; a scene script (`game\scene\...`: ambient loops,
 * effects) is only counted. */
void rb_pcs_begin(rb_t *b, uint32_t key, int real_cutscene);
int rb_pcs_is_cutscene(const char *resource);
void rb_movie_begin(rb_t *b, uint32_t key);          /* FUN_00785580 ran (log / count only) */
/* the mPcsIsEnd getter: engine_ended is what the game computed; the return is what the script should see */
int rb_pcs_end(rb_t *b, int engine_ended);
/* the whole POST filter: the getter's full eax in, the eax to return out; bit for bit the same unless the end is held (then the low byte becomes 0) */
uint32_t rb_pcs_filter(rb_t *b, uint32_t engine_eax);

/* the level load barrier, once per frame. playable: this PC has its own human unit and a normal camera (not loading), and the unit is placed (rb_placed). */
void rb_load_step(rb_t *b, int playable);
/* (e1w2 live, 2026-11-08) the own human is placed: a finite position away from 0,0,0. After FUN_00720870 the game creates the player units within ~0.4 s at
 * 0,0,0 and keeps them there for the whole load / start movie (1-1: ~110 s, 1-2: ~3.5 min): "unit exists" alone opened the barrier at once on the first load
 * and never saw a level go away at an in-session restart (the units are replaced in < unplay_ms), so the world model kept one epoch for 1-1 and 1-2. */
int rb_placed(float x, float y, float z);

/* ---------------------------------------------------------------- 4. restart */
int rb_triple_ok(const rb_triple *t, char *why, size_t wsz);       /* plausible: a campaign area (0x91EESS00), checkpoints not both zero and in range */
/* host: the engine is about to restart with t; the triple goes to the client as a RESTART event. 1 = sent. */
int rb_restart_host(rb_t *b, const rb_triple *t);
/* client: the engine is about to restart with `inout` (its own save); 1 = replaced by the host's triple, 0 = left alone (b->why says why), -1 = answer "not now" (hold) */
int rb_restart_client(rb_t *b, rb_triple *inout);
void rb_restart_inject(rb_t *b, const rb_triple *t);               /* console: pretend the host sent t */
/* client, once per frame (world-downed, wp1 live: the host's Continue reloaded the host only; the client, whose partner human has no game-over screen, never called
 * its own Continue and stayed in the old level instance). 1 = the glue should call the engine's restart FUN_00720870 with *out now: a fresh host triple is waiting,
 * no own Continue used it for cfg.follow_ms, and this triple was not tried yet. The hook of that very call then takes the triple (rb_restart_client). */
int rb_restart_follow(rb_t *b, rb_triple *out);
/* lobby-must: the same at the title screen (no level on this PC: the guest after START, or a late guest): no upper age limit, the host's newest triple is
 * the story to load (the guest's own menu is held, so no own Continue can come) */
int rb_restart_follow_at(rb_t *b, rb_triple *out, int at_title);
/* the answers of rb_restart_follow_at: 0 nothing now, RB_FOL_CALL = call FUN_00720870 with *out now, RB_FOL_SECEND = set this PC's section-end game flags
 * now (rev1 live 10: the stock section end of the section the host left; once per SECTION; rb_section_change consumes the host's triple when it comes) */
enum { RB_FOL_NONE = 0, RB_FOL_CALL = 1, RB_FOL_SECEND = 3 };
/* rev1 live 15: the guest's own save read before its follow at the title (cfg.title_load). The glue calls rb_title_load_gate when rb_restart_follow_at said
 * RB_FOL_CALL (or the title retry wants to call again): RB_TL_CALL = call FUN_00720870 now (no read: not at the title, the host, the feature off, not a SAVE
 * start, or this title stay has read already); RB_TL_LOAD = ask the stock save read now and call later (rb_title_load_asked tells whether it could be asked);
 * RB_TL_WAIT = a read runs: no call now. rb_title_load_frame once per frame: 1 = the read is over (done, failed or timed out): call FUN_00720870 with *out
 * now; leaving the title forgets the state (the next title stay reads again). */
enum { RB_TL_CALL = 0, RB_TL_LOAD = 1, RB_TL_WAIT = 2 };
enum { RB_TLS_IDLE = 0, RB_TLS_ASK = 1, RB_TLS_WAIT = 2, RB_TLS_DONE = 3 };
int rb_title_load_gate(rb_t *b, const rb_triple *fol, int at_title);
void rb_title_load_asked(rb_t *b, int asked, const char *why);
int rb_title_load_frame(rb_t *b, int at_title, int done, int ok, rb_triple *out);
int rb_start_kind(uint32_t ret_addr);   /* RB_KIND_* of a FUN_00720540 caller (the host's story start) */
int rb_msg_restart_k(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b, int diff, const rb_cp *cp, int kind);   /* RB_RESTART_K_LEN or 0 */
#define RB_SECEND_GIVEUP_MS 15000u   /* no own section change this long after the flags: the restart call after all (the glue clears the flags first) */
/* The section-end game flags: the final script of every Episode 1 section (12_00_event_s200_obr n3, 13_060_0's script, 11's) runs
 * FsmSetGameFlag(2021257430, 2, set) and FsmSetGameFlag(505085882, 2, set); FsmSetGameFlag 0x747880 = FUN_0078EB80([0x1169110], type)->vf+0x14(no).
 * Bit 2 of the first ([0x113AFA0]) makes cEpisodeMainState's update (0x42CE90) enter cEpisodeEndState (FUN_0042E890(3)); the second ([0x113AFA4]) is the
 * cut flow's own end bit. Both are reset by the next area's cEpisodeProcessState unless the restart bit is set (FUN_0042DE90). */
#define RB_GF_TYPE_EPISODE 2021257430u
#define RB_GF_TYPE_CUT 505085882u
#define RB_GF_SECTION_END 2u

/* lobby-must: the host's story start. Results of rb_start_triple / rb_restart_host (besides 1 = sent, 0 = not sent): */
enum { RB_ST_NONE = 0, RB_ST_SENT = 1, RB_ST_DUP = 2, RB_ST_BAD = -3, RB_ST_NOTSEC = -4 };   /* (allep1: NOTSEC / SOLO = the id is the solo 16-2 / no section: not sent, the glue ends the session with its message) */
#define RB_SECTION_FN_LO 0x42D160u   /* cEpisodeEndState_vf7: its calls of FUN_00720540 are the section change, not a start */
#define RB_SECTION_FN_HI 0x42D6E8u
/* allep1 (episode-catalog.md, FACT live): the 29 story sections have an area class; 28 start with two playable characters (PAIR), 16-2 (0x91100200, Parker alone, the extra
 * episode) is SOLO. 0-1 / 0-2 / 8-4 (0x91000100 / 0x91000200 / 0x91080400) have no area class (warp falls back to the Episode 1 start) and F1 (0x910F0100) makes the game exit:
 * NOTSEC. RB_SK_END = the campaign is over (0x91FF0000 "no next section", or an id that is no 0x91 campaign id); the real area after 12-3 is 0x910E0000 (NOTSEC, allep4s). */
/* RB_SK_* (secids1.h) */
int rb_section_kind(uint32_t area);
/* the area may be played and followed in a session: a paired section of any episode */
int rb_area_ok(uint32_t area);
/* allep1: how an end of co-op is named (rb_t.ep_kind, RBM_EPDONE carries only `from`): RB_EK_*; derived from the finished section when
 * the PC only knows the partner's EPDONE: the last section of the story (12-3) = the campaign, 16-1 = the solo part */
enum { RB_EK_NONE = 0, RB_EK_CAMPAIGN = 1, RB_EK_NOTSEC = 3 };
int rb_end_kind_from(uint32_t from);
/* host, the observer of FUN_00720540: 1 = this call is a story start of this PC (the glue reads the start record in its next frame), 0 = a section change / not host */
int rb_start_seen(rb_t *b, uint32_t ret_addr);
/* host, a frame after rb_start_seen: the triple of the start record. RB_ST_SENT, RB_ST_DUP (the restart hook sent the same within 3 s), RB_ST_NOTSEC / RB_ST_SOLO (the id is no paired section:
 * not sent, the glue ends the session), RB_ST_BAD, RB_ST_NONE (nothing pending). */
int rb_start_triple(rb_t *b, const rb_triple *rec);
/* lm2 / lm2b live (the title's New Game on the host): after FUN_00720540 the global start record holds the START id 0x92010100 with checkpoints 256 / 1, not
 * a 0x91 area ("area is not a campaign section id"), so nothing was sent and the guest stayed at the title. The restart the guest needs is the one of the title
 * Continue of a 1-1 save: FUN_00720870(0x91 area, the start table's checkpoints) (lm2b: the guest's FUN_00720870(91010100, 256, 1) at the title loaded 1-1).
 * Read from the start table directly: [[gamesys_ptr]+0x24] = o; table = [o+0x20] == 0 ? [o+0xC] : [o+0x10] (0x720573); FUN_00726B70: [table+0x6C] entries at
 * [table+0x78], key = entry+4; checkpoints = entry+0xC / +0x10 (0x720601). *out = {area, A, B}. rd = a safe 32-bit read. 1 = the area's entry was found. */
typedef uint32_t (*rb_rd32_fn)(void *user, uint32_t addr);
int rb_table_triple(rb_rd32_fn rd, void *user, uint32_t gamesys_ptr, uint32_t area, rb_triple *out);
/* wp6 live (host crash exe+2c9700 at the game-over Continue): the lab `packcopy` copies the words +4..+0x140 of one human's cBioItemPack (unit+0xFE8) into the
 * other's, so the SAME weapon objects sit in two bags; the reload's teardown releases them twice and the unit manager's list ([0x11695C8] +0x34..+0x38, walked by
 * FUN_006C96E0 when the next weapon / bullet is created) is left with a dead node (stage6 #87: the same class after a weapon-owner transfer). Never in the
 * product (nothing of the mod writes a bag). The check before every restart: the weapon objects (is_obj) that both bags hold; *out gets up to max of them.
 * Returns how many. */
#define RB_BAG_FIRST 4u
#define RB_BAG_END 0x144u
typedef int (*rb_isobj_fn)(void *user, uint32_t v);
int rb_bag_shared(rb_rd32_fn rd, rb_isobj_fn is_obj, void *user, uint32_t bag_a, uint32_t bag_b, uint32_t *out, int max);

/* ---------------------------------------------------------------- 5. ep1-flow: the section change (E4) and the end of co-op (E5)
 * E4 (gap table of notes\ep1-coop-map.md): before this, each PC changed the section by itself after the `epend` rendezvous (180 s cap), so a PC that never reached
 * the end stayed in the old section. Now the HOST's own section change (cEpisodeEndState_vf7 calls FUN_00720540(record, next): 1-1 -> 0x91010200, 1-2 ->
 * 0x91010300) sends the next section's start triple (the start table's checkpoints: (91010200, 512, 0), (91010300, 256, 2)). The client follows it with the
 * Continue follow (rb_restart_follow_at -> FUN_00720870 through the restart hook) unless it changes to the same section by itself: it is at its own section end
 * (its rendezvous ran in the last sec_wait_ms) or its own section change to that area came within sec_window_ms of the triple.
 * E5: the end of 1-3 is a section change OUT of Episode 1 (FUN_0078FCB0 gives the next episode: FUN_00720540(record, 0x91020100) from 0x42D4AD, then the stock
 * aEpisodeEnd flow: result, save, then aEpisodeTitle of Episode 2). Each PC that passes it tells the other (RBM_EPDONE); no triple is followed after it; the
 * session closes on each PC after its own stock end flow (rb_ep_close_step), so 2-1 never loads inside the session. */
int rb_msg_section(uint8_t *out, size_t cap, uint16_t seq, const rb_triple *next, uint32_t from, int how);   /* RB_SECTION_MSG_LEN or 0 */
int rb_msg_epdone(uint8_t *out, size_t cap, uint32_t from);                                                    /* RB_EPDONE_MSG_LEN or 0 */
enum { RB_SC_NONE = 0, RB_SC_SENT = 1, RB_SC_EPEND = 2, RB_SC_NOTED = 3, RB_SC_BAD = -1 };
/* this PC's own section change (both roles; the glue calls it from the `start` observer when the caller is cEpisodeEndState_vf7): from = the section that ended
 * (the running area's hash, 0 = unknown), next = FUN_00720540's area, next_start = the start table's triple of next (NULL = not found).
 * RB_SC_SENT host: RBM_SECTION sent; RB_SC_EPEND the end of co-op (RBM_EPDONE sent, the follow is off from now on); RB_SC_NOTED client: noted (the host's
 * triple to the same area is consumed); RB_SC_BAD host: no start triple for next (nothing sent, logged); RB_SC_NONE: no session / not a paired section. */
int rb_section_change(rb_t *b, uint32_t from, uint32_t next, const rb_triple *next_start);
/* E5, once per frame while this PC's own end of Episode 1 is known: in_end_flow = the running area is the stock episode end (aEpisodeEnd), level_out = a level
 * with player units whose area is not a section of Episode 1 (2-1 loaded: the latest point). 1 once = close the session now (b->why says why):
 * the end flow was seen and has ended (aEpisodeTitle of Episode 2 comes), or level_out, or ep_close_cap_ms passed. */
int rb_ep_close_step(rb_t *b, int in_end_flow, int level_out, uint32_t now);
int rb_ep_known(const rb_t *b, int *local);   /* 1 = the end of Episode 1 is known here (own or the partner's RBM_EPDONE); *local = this PC passed it itself */

/* ---------------------------------------------------------------- 6. continue1: the Continue after a shared game over
 * (A) e1p6: the host's stock Continue reloaded in place in 2 s; the guest's follow (a direct FUN_00720870) reloaded the section with its opening movie 11_010_0
 *     (112 s). The area start FUN_0041c8d0 skips that movie only when the campaign checkpoint record says a checkpoint was passed (+0x18 > 0, with the restart bit
 *     FUN_00720870 sets); the guest's record still said 0 (its own pass of checkpoint mID 1 had not been written), the host's said 1. Now RESTART carries the host's
 *     record and the guest's restart hook writes it into its own record before the engine reloads: the same movie decision, the same checkpoint on both PCs. */
int rb_msg_restart_cp(uint8_t *out, size_t cap, uint16_t seq, uint32_t area, int32_t a, int32_t b, int diff, const rb_cp *cp);   /* cp->have: 21 bytes, else as rb_msg_restart_d */
/* FUN_0041c8d0's decision (0x41C97F..0x41C9B4): 1 = the area's start movie plays. movie_done = flag set [0x113AFA0] bit 8, restart = flag set [0x113AF8C] bit 1 */
static inline int rb_start_movie_plays(int movie_done, int restart, int32_t cp_id) { return !movie_done && !(restart && cp_id > 0); }
/* the record slot as 9 words (0x24 bytes: +8 = w[2], +0x10 = w[4], +0x14 area w[5], +0x18 mID w[6], +0x1C A w[7], +0x20 B w[8]): the host's checkpoint into it.
 * Returns 1 when a word changed. The bit 1 of +8 is cleared as FUN_006d1a70 does when a checkpoint is passed. */
int rb_cp_rec_take(uint32_t w[9], const rb_triple *t, const rb_cp *cp);
/* (B) the level start of a restart: the aCut area's load step FUN_0041cfc0 answered "ready" (engine_ready). RB_HOLD = answer "not ready" this frame: this PC waits
 *     at the end of its load (no units placed, no enemies, no scene script yet) until the other PC's level is ready too (RB_LSTART, key = the RESTART's number), at most
 *     cfg.lstart_ms. Armed by the host's restart (rb_restart_host) and by the guest's restart with the host's triple (rb_restart_client). */
int rb_lstart_gate(rb_t *b, int engine_ready);
/*     the shield: 1 = the own humans must take no hit now (a restart came and the session's barrier of the next level instance has not opened yet; at most
 *     cfg.shield_ms). The glue drops the enemies' blows on its own humans meanwhile (the host's ApplyHit, the client's PCHIT). */
int rb_shield_on(rb_t *b);
/* (C) the follow goes through this PC's own stock Continue (FUN_0041efe0(0): aStartContinue -> its checkpoint read -> FUN_0041c8d0 / FUN_00720870 from
 *     FUN_006d0cb0, rewritten by the restart hook) when rb_restart_follow_at said "follow" with b->fol_try_stock = 1 and the glue's checks of the engine state pass.
 *     The glue tells which way it went; a stock Continue that has not used the triple after cfg.stock_wait_ms gets the direct call (rb_restart_follow_at again). */
void rb_follow_went(rb_t *b, int stock);
/* (D) the stock Continue request FUN_0041efe0 ran on this PC (the RBK_CONT PRE gate). ours = the mod asked it (the follow / the answer to the guest). RB_HOLD = drop
 *     this call (a request within cfg.cont_dedup_ms and no level loaded since: a second aStartContinue would assert in FUN_006d0ff0). On the guest in a session, a
 *     request of its own UI (game-over Yes, pause menu) while no host triple waits sends RBM_CONTREQ: the host continues, the guest's restart then waits for it. */
int rb_cont_request(rb_t *b, int ours);
/*     host, once per frame: 1 = the guest asked for a Continue: the glue asks its own stock Continue now (when the engine state allows; rb_cont_answered tells) */
int rb_cont_request_step(rb_t *b);
void rb_cont_answered(rb_t *b, int done, const char *why);
/*     client, once per frame while its own Continue's restart is held ("not now"): 1 = call FUN_00720870 with *out now (the host's triple came: *host = 1; or the
 *     wait is over: its own checkpoint, *host = 0); 0 = keep waiting / nothing held */
int rb_held_step(rb_t *b, rb_triple *out, int *host);

/* ---------------------------------------------------------------- 7. continue1 task 4 (Q8): the checkpoint pass mirror
 * e1p2: the guest's record was CLEARED (0x91FF0000 / -1) when the mirrored 4390 came before its movie ended; e1p5 / e1p6 / e1p9: the guest's record stayed at the
 * level start's mID 0 (FUN_006d1e90 passes a mID 0 content with the start triple) while the host passed CP mID 1 (its own Jill in the CheckPoint sphere at 4390),
 * so the guest had neither the record nor the checkpoint save of mID 1. Now each PC tells the other when its record reaches a new checkpoint (mID > 0, RBM_CPPASS),
 * and the other PC passes the same checkpoint the engine's own way (FUN_006d1a70 with a cZoneContentsCheckPoint built like FUN_006d1e90 does: its record and its
 * checkpoint save of its own human), unless it has it already. The engine refuses a checkpoint already passed (the mask bit, mFlag bit 0 clear), so a late or
 * stale pass never goes back. */
#define RB_CPPASS_MSG_LEN 16
enum { RBM_CPPASS = 6 };
int rb_msg_cppass(uint8_t *out, size_t cap, uint16_t seq, const rb_cprec *r);   /* RB_CPPASS_MSG_LEN or 0 */
/* once per frame (both roles): own = this PC's record now (have 0 = not readable), in_level = a level of the session is playable here. 1 = pass the partner's
 * checkpoint *out now (the glue calls FUN_006d1a70; rb_cpm_applied tells how it went), 0 = nothing to do. Sends this PC's own new passes. */
int rb_cpm_step(rb_t *b, const rb_cprec *own, int in_level, rb_cprec *out);
void rb_cpm_applied(rb_t *b, int called, const rb_cprec *after);
/* a record write of the mod itself (the restart's cp_take): not a pass of this PC to tell the partner about */
void rb_cpm_mark(rb_t *b, const rb_cprec *r);

/* the hash used for names (FNV-1a, 32 bit; "" = 0) */
uint32_t rb_hash(const char *s);

/* status text */
void rb_status(const rb_t *b, char *out, size_t size);

/* rolebarrier1_game.c (the game glue, game thread): once from probe_install; the console command `barrier status|apply|revert|auto|restart|set` */
int rolebarrier_init(void);
void rolebarrier_cmd(const char *arg, char *out, int size);
/* ep1-flow E5: GetTickCount | 1 when the end of Episode 1 became known here (this PC's own end or the partner's EPDONE; 0 = not, or older than 40 min);
 * *local = this PC passed it itself; *closed = when this PC closed the session for it (0 = not yet). menu.c: the strip, and no loss strip / title return for it. */
unsigned rolebarrier_ep1_end(int *local, unsigned *closed);
/* allep1: GetTickCount | 1 when this PC (host) started the solo 16-2 / a non-section id with the room option on and the session was closed (*solo = the solo part); 0 = never */
unsigned rolebarrier_sec_block(int *solo);
int rolebarrier_spectating(unsigned *watched);   /* allep1: 1 = this PC spectates in the solo 16-2 (no unit of its own); *watched = the unit it watches */
int rolebarrier_end_kind(void);   /* allep1: RB_EK_* of the end of co-op that became known here */
/* lab tools that edit units in memory (probe.c `packcopy`) register an undo here: it runs on the game thread right before FUN_00720870 reloads the level
 * (the host's Continue / game-over "Yes", the guest's own, the follow), i.e. before the teardown that would free shared objects twice (wp6). Only while the
 * restart hook is in force (a network session, REV1_BARRIER=1 or `barrier apply`). At most 4. */
void rolebarrier_on_restart(void (*cb)(void));
/* continue1: 1 = this PC's own humans must take no enemy blow now (a restart came, the session barrier of the next level instance has not opened); any thread */
int rolebarrier_shielded(void);
void rolebarrier_shield_drop(int pchit);   /* the glue of a dropped blow counts it here (world1_game.c: 0 = ApplyHit on the host, 1 = a PCHIT on the client) */

#endif
