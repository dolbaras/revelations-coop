/*
 * rolebarrier1_game.c - the glue of the role / barrier / restart logic (rolebarrier1.c) to the running game: process memory, the network session of net1.c, the own slot of capture.c,
 * the engine detours, one pass per logic frame and the console command `barrier`. Background and the list of live checks: notes\net-role-barrier.md.
 *
 * OFF by default. The detours are written (after their bytes were checked) when a network session has STARTED (net1_session_active) or REV1_BARRIER=1, or by the console (`barrier apply`),
 * and taken out again when the session ends. A refusal (the bytes are not those of build 13298487) is logged with the reason and not retried every frame. REV1_BARRIER_OFF=pcsend,movie
 * leaves single hooks out. With REV1_BARRIER=1 and no session the hooks only LOG what the engine does (cutscene names, movies, restarts): the first live check of every assumption.
 *
 *   barrier status                       state of every hook (live bytes, calls, calls that CHANGED the engine, last refusal), the rendezvous and load counters, the role
 *   barrier apply <hook>|hooks           this hook (epend|pcs|movie|pcsend|restart) / all five forced ON; the role and the load barrier are not touched
 *   barrier revert <hook>|hooks          this hook / all five forced OFF (original bytes); the role and the load barrier are not touched  (per-hook A/B)
 *   barrier role off|auto                the role of the joined player (capture_set_own / capture_bind at START) off / back to automatic; hooks not touched
 *   barrier load off|auto                the automatic LOAD_BEGIN / LOAD_DONE (session load barrier) off / automatic; then `net load` / `net done` by hand
 *   barrier apply | revert               (no argument) EVERYTHING on / off: hooks, rendezvous, load barrier AND the role (the 2026-11-06 duo4 A/B used this one)
 *   barrier auto                         back to "a network session decides" for everything
 *   barrier restart inject <hex area> <a> <b>          client test: pretend the host sent this triple (the next Continue is rewritten with it)
 *   barrier restart call <hex area> <a> <b> yes        lab: call the engine's restart (FUN_00720870) with this triple through the normal path (REAL: it changes the level)
 *   barrier set section|pcs|notriple|latch <ms>        timeouts (notriple: a client Continue without a host triple answers "not now" for that long; 0 = use its own)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "r1.h"
#include "capture.h"
#include "net1.h"
#include "rolebarrier1.h"

#define CAMROOT_PTR 0x11E00B8u
#define PCSMAN_PTR 0x116A34Cu     /* [..] = sPcsManager */
#define EPISODE_PTR 0x116A340u    /* [..] = the object FUN_00720870 is a method of (every caller loads it into ecx) */
#define GAMESYS_PTR 0x1169108u    /* [[..]+0x24] = the object whose FUN_008C66B0 gives the start record: [[[..]+0x24]+4] = record, +4 area, +8 checkpoint A, +0xC B (720870 / 720540 write it) */
#define TC __attribute__((thiscall))
/* continue1 (notes\continue1.md section 1) */
#define SAVEMAN_PTR 0x116A328u    /* [..] = sSaveManager: +0xb4 != 0 = a save / load job runs, +0xe0 = the save cache slot (-1 = none: FUN_006d0ff0 asserts) */
#define CHECKPT_PTR 0x116A334u    /* [..] = sCheckPoint: +0x5c bit 0 = a checkpoint read is pending; the campaign record at +0x60 + mode * 0x24 */
#define CONT_RET_STOCK1 0x422777u /* FUN_0041efe0's callers in the area's request handler FUN_004225a0 (case 3 = the stock Continue, case 5/6 = Raid co-op) */
#define CONT_RET_STOCK2 0x4229E5u

_Static_assert(CP_EV_RB_KIND == CP_EV_USER + 0x41, "event kind");

static rb_t B;
static rb_role_t R;
static rbk_state HS[RBK_N];
static int inited, env_on, mode;   /* mode: 0 auto, 1 on, 2 off (everything) */
static int hm[RBK_N];              /* per hook: 0 auto (follows the session), 1 forced on, 2 forced off */
static int skipped[RBK_N], hook_last[RBK_N], hooks_last;
static int role_mode, load_mode;   /* 0 auto, 2 off */
static int changing[RBK_N];        /* the last call of this hook changed the engine (for the episode count) */
static CRITICAL_SECTION cs;
static lk_t *reg_link;
static int g_role_pad_checks;
static int g_role_own_fixes;   /* wp12: the partner's own slot put back after something else took it (the lobby close did, fixed in menu.c) */
static unsigned g_logged[RBK_N];
/* lobby-must: the Episode 1 gate (the host started outside Episode 1: the session ended) and the client's follow at the title */
static volatile unsigned g_ep_block_ms;
static int g_ep_stop;              /* the frame must close the session (outside the lock) */
static rb_triple g_fol_last;       /* the follow at the title: the last triple tried and when (a refused call is tried again while no level loaded) */
static unsigned g_fol_t, g_fol_tries;
static int g_gf_set;               /* rev1 live 10: this PC set its section-end game flags for a SECTION follow (cleared before a fallback restart call) */
static unsigned g_gf_sets, g_gf_fails, g_gf_clears;
static uint32_t g_st_area;         /* the area argument of the pending story start (FUN_00720540): the start table's key when the record is gone */
/* ep1-flow E5: the end of Episode 1 (kept outside B: rb_reset clears B when the session closes, the strips come after that) */
static volatile unsigned g_ep1_t;       /* GetTickCount | 1 when the end of Episode 1 became known here (this PC's own end or the partner's EPDONE); 0 = not */
static volatile int g_ep1_local;        /* this PC passed it itself (its own stock end flow runs: result, save, Episode 2) */
static volatile unsigned g_ep1_closed;  /* GetTickCount | 1 when this PC closed the session for it */
static int g_ep1_close;                 /* the frame must close the session (outside the lock) */
static char g_ep1_area[48];             /* the running area class at the close (log) */
static int g_was_active;                /* the session was started in the previous frame (a new START forgets the last session's rendezvous) */
/* continue1 */
static volatile int g_shield;           /* rb_shield_on of the last frame: the own humans take no enemy blow (read by world1_game.c from any thread) */
static volatile unsigned g_shield_drops[2];
static int g_cont_ours;                 /* set around the mod's own call of the stock Continue FUN_0041efe0 (its hook tells the library) */
static const char *g_cr_why;            /* the last reason the host could not answer the guest's Continue request yet (logged once) */

/* ---------------------------------------------------------------- memory */
static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((void *)(size_t)a, &v, 4);
    return v;
}

static int m_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    return r1_safe_read((const void *)(uintptr_t)a, out, n);
}
static int m_write(void *u, uint32_t a, const void *p, uint32_t n)
{
    (void)u;
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)(uintptr_t)a, p, n);
    VirtualProtect((void *)(uintptr_t)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)a, n);
    return 1;
}
static uint32_t m_alloc(void *u, uint32_t n)
{
    (void)u;
    return (uint32_t)(uintptr_t)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);   /* 32-bit process: the address fits */
}
static void m_log(void *u, const char *line)
{
    (void)u;
    r1_log("%s", line);
}
static const rbk_mem MEM = {m_read, m_write, m_alloc, m_log, NULL};

static int is_player_class(unsigned unit)
{
    char cn[96];
    return unit > 0x10000 && classes_object_name(unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4);
}
static unsigned unit_by_slot(unsigned slot)
{
    unsigned sp = rd32(R1_SPLAYER_PTR);
    if (!sp) return 0;
    unsigned head = rd32(sp + 0x30), endp = rd32(sp + 0x34);
    for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8)) {
        unsigned unit = rd32(head + 0x10);
        if (unit && rd32(unit + 0xE2C) == slot && is_player_class(unit)) return unit;
    }
    return 0;
}

/* a printable C string of at least 3 characters at addr (the name of a cutscene / movie), else "" */
static int read_name(unsigned addr, char *out, int size)
{
    out[0] = 0;
    if (addr < 0x10000) return 0;
    char tmp[48];
    memset(tmp, 0, sizeof tmp);
    if (!r1_safe_read((void *)(size_t)addr, tmp, sizeof tmp - 1)) return 0;
    int n = 0;
    while (n < (int)sizeof tmp - 1 && tmp[n] >= 0x20 && tmp[n] < 0x7F) n++;
    if (n < 3 || (n < (int)sizeof tmp - 1 && tmp[n] != 0)) return 0;
    snprintf(out, (size_t)size, "%.*s", n, tmp);
    return 1;
}

/* ---------------------------------------------------------------- the session */
static int io_session(void *u, rb_sinfo *o)
{
    (void)u;
    lk_t *k = net1_link_peek();
    if (!k || !net1_session_active()) return 0;
    lk_info in;
    lk_get_info(k, &in);
    o->started = in.started && in.remote_role >= 0, o->phase = in.phase, o->epoch = (unsigned)in.epoch, o->gos = in.gos;
    o->other_area = in.peer_load_area, o->other_area_epoch = (unsigned)in.peer_load_epoch;   /* rev1 live 8: the cross-level guard of rb_load_step */
    /* lobby-must (S3): a partner who left mid-section leaves the host alone in a "started" session: nothing may be held for him any more (a section end would wait
     * 180 s), no triple is sent to nobody */
    return o->started;
}
static int io_send(void *u, const void *body, unsigned len)
{
    (void)u;
    lk_t *k = net1_link_peek();
    cs_t *s = k ? lk_session(k) : NULL;
    return s && cs_send_event(s, CP_EV_RB_KIND, body, len);
}
static void io_begin(void *u, uint32_t key)
{
    (void)u;
    lk_t *k = net1_link_peek();
    if (k) lk_load_begin(k, key);
}
static void io_done(void *u)
{
    (void)u;
    lk_t *k = net1_link_peek();
    if (k) lk_load_done(k);
}
static uint32_t io_now(void *u)
{
    (void)u;
    return GetTickCount();
}
static void io_log(void *u, const char *line)
{
    (void)u;
    r1_log("%s", line);
}

/* events of the other PC arrive on the session's thread (lobby_link's callback, session lock held): only queued here */
typedef struct { uint8_t len; uint8_t body[RB_MSG_MAX]; } evq_t;
static evq_t EQ[32];
static int eq_head, eq_tail;
static CRITICAL_SECTION eq_cs;
static int eq_ready;
static void on_event(void *user, const cp_event *e)
{
    (void)user;
    if (!eq_ready || e->kind != CP_EV_RB_KIND || e->len == 0 || e->len > RB_MSG_MAX) return;
    EnterCriticalSection(&eq_cs);
    int next = (eq_head + 1) % 32;
    if (next != eq_tail) {
        EQ[eq_head].len = e->len;
        memcpy(EQ[eq_head].body, e->body, e->len);
        eq_head = next;
    }
    LeaveCriticalSection(&eq_cs);
}
static void drain_events(void)
{
    for (;;) {
        evq_t ev;
        EnterCriticalSection(&eq_cs);
        int have = eq_tail != eq_head;
        if (have) ev = EQ[eq_tail], eq_tail = (eq_tail + 1) % 32;
        LeaveCriticalSection(&eq_cs);
        if (!have) break;
        rb_on_event(&B, ev.body, ev.len);
    }
}

/* ---------------------------------------------------------------- the detour callbacks (any game thread; one lock) */
/* one call of a hook: count it, and when it changed what the engine does, count that too (an unbroken stretch of changed calls = one episode, logged at its start and end) */
static void mark(int which, int changed, const char *what)
{
    HS[which].calls++;
    if (changed) {
        HS[which].changed++;
        if (!changing[which]) {
            HS[which].episodes++;
            r1_log("barrier: hook %s CHANGES the engine (episode %u): %s", rbk_defs[which].name, HS[which].episodes, what);
        }
    } else if (changing[which])
        r1_log("barrier: hook %s back to the engine's own behaviour after %u changed calls in all (%s)", rbk_defs[which].name, HS[which].changed, what);
    changing[which] = changed;
}

/* the area hash of an object whose vtable slot 4 is its DTI getter (`mov eax, DTI; ret`), hash at DTI+0x1C; 0 = not readable */
static unsigned obj_area_hash(unsigned obj)
{
    unsigned vt = obj > 0x10000 ? rd32(obj) : 0, fn = vt ? rd32(vt + 16) : 0;
    unsigned char c[6] = {0};
    if (fn < 0x10000 || !r1_safe_read((void *)(uintptr_t)fn, c, 6) || c[0] != 0xB8 || c[5] != 0xC3) return 0;
    unsigned dti;
    memcpy(&dti, c + 1, 4);
    return rd32(dti + 0x1C);
}
static volatile unsigned g_vf7_from;   /* ep1-flow: the finished section as cEpisodeEndState_vf7 reads it: [[arg0]+4] -> vf+0x10 (DTI) -> +0x1C (0x42D3CB) */
/* the section of an area hash: 0x91EESS00 (aEpisodeNN), or 0x92EESSxx = the running cut area aCutEESS (q7 live: `menu net` named aCut1100 / aCut1200 / aCut1300
 * in 1-1 / 1-2 / 1-3; work\dti-rev1.txt: aCut1100 hash 92010100, aCut2100 92020100) -> 0x91EESS00; anything else 0 */
static unsigned sec_of(unsigned h)
{
    if ((h >> 24) == 0x91u) return h & 0xFFFFFF00u;
    if ((h >> 24) == 0x92u) return 0x91000000u | (h & 0x00FFFF00u);
    return 0;
}

static uint32_t __attribute__((force_align_arg_pointer)) cb_epend(rbk_frame *f)
{
    unsigned param = f->arg[0], from = param > 0x10000 ? obj_area_hash(rd32(param + 4)) : 0;
    g_vf7_from = from;
    EnterCriticalSection(&cs);
    rb_section_end_seen(&B);
    int hold = rb_section_gate(&B) == RB_HOLD;   /* never holds (ef4: vf7 runs once; the early return dropped the host's section change for good) */
    mark(RBK_EPEND, hold, hold ? "early return from cEpisodeEndState_vf7 (section end held)" : "section end goes on");
    if (!g_logged[RBK_EPEND]++) r1_log("barrier: epend: the section end state runs for the first time (%s), finished section %08x", hold ? "held" : "goes on", from);
    else if (!hold) r1_log("barrier: epend: the section end of %08x goes on (%s)", from, B.why);
    LeaveCriticalSection(&cs);
    return (uint32_t)hold;
}

static uint32_t __attribute__((force_align_arg_pointer)) cb_pcs(rbk_frame *f)
{
    char name[56], where[16] = "";
    unsigned obj = f->arg[0];
    if (!read_name(obj + 8, name, sizeof name)) {
        if (read_name(obj, name, sizeof name)) snprintf(where, sizeof where, "@0");
        else {
            unsigned p = rd32(obj + 8);
            if (!read_name(p, name, sizeof name)) name[0] = 0;
            else snprintf(where, sizeof where, "@*8");
        }
    } else
        snprintf(where, sizeof where, "@8");
    int real = rb_pcs_is_cutscene(name);
    EnterCriticalSection(&cs);
    rb_pcs_begin(&B, rb_hash(name), real);
    mark(RBK_PCS, 0, "observer");
    if (g_logged[RBK_PCS] < 40) {
        g_logged[RBK_PCS]++;
        r1_log("barrier: pcs #%u: %s starts: name '%s'%s arg0 %08x user %08x, this %08x", HS[RBK_PCS].calls, real ? "CUTSCENE (its end meets the other PC)" : "scene script (counted only)", name, where, obj,
               f->arg[1], f->ecx);
    }
    LeaveCriticalSection(&cs);
    return 0;   /* observer: the stub has no early return, the value is not used */
}

static uint32_t __attribute__((force_align_arg_pointer)) cb_movie(rbk_frame *f)
{
    char name[56];
    if (!read_name(f->arg[0], name, sizeof name)) name[0] = 0;
    EnterCriticalSection(&cs);
    rb_movie_begin(&B, rb_hash(name));
    mark(RBK_MOVIE, 0, "observer");
    if (g_logged[RBK_MOVIE] < 40) {
        g_logged[RBK_MOVIE]++;
        r1_log("barrier: movie #%u: movie starts: name '%s' stream %u endStatus %u stopBgm %u/%u, this %08x, from %08x", HS[RBK_MOVIE].calls, name, f->arg[1], f->arg[2], f->arg[3] & 0xFF, f->arg[4] & 0xFF,
               f->ecx, f->ret_addr);
    }
    LeaveCriticalSection(&cs);
    return 0;
}

static uint32_t __attribute__((force_align_arg_pointer)) cb_pcsend(uint32_t self, uint32_t result)
{
    if (self != rd32(PCSMAN_PTR)) return result;   /* not the cutscene manager's own property: untouched */
    EnterCriticalSection(&cs);
    uint32_t r = rb_pcs_filter(&B, result);
    char what[96];
    snprintf(what, sizeof what, "mPcsIsEnd: the game says %08x, the script gets %08x", result, r);
    mark(RBK_PCSEND, r != result, what);
    LeaveCriticalSection(&cs);
    return r;
}

static void (*g_restart_cbs[4])(void);
static int g_n_restart_cbs;
void rolebarrier_on_restart(void (*cb)(void))
{
    if (cb && g_n_restart_cbs < 4) g_restart_cbs[g_n_restart_cbs++] = cb;
}

static uint32_t rd32_u(void *user, uint32_t a)
{
    (void)user;
    return rd32(a);
}
static int is_weapon_obj(void *user, uint32_t v)   /* the weapons packcopy hands over (uWp*, not the bare-hand / ground ones) */
{
    (void)user;
    char cn[96];
    return classes_object_name(v, cn, sizeof cn) && !strncmp(cn, "uWp", 3) && strncmp(cn, "uWpUnarmed", 10) && strncmp(cn, "uWpGndHand", 10) && strncmp(cn, "uWpGndPulse", 11);
}

/* before the engine reloads: the lab tools' undo, then the bag check of the two humans (wp6: shared weapons crash the reload's teardown) */
static void before_restart(void)
{
    for (int i = 0; i < g_n_restart_cbs; i++) g_restart_cbs[i]();
    unsigned u0 = unit_by_slot(0), u1 = unit_by_slot(1);
    unsigned b0 = u0 ? rd32(u0 + 0xFE8) : 0, b1 = u1 ? rd32(u1 + 0xFE8) : 0;
    uint32_t sh[4];
    int n = rb_bag_shared(rd32_u, is_weapon_obj, NULL, b0, b1, sh, 4);
    if (n > 0)
        r1_log("barrier: WARNING before the reload: the two humans' bags (%08x / %08x) hold %d weapon object(s) both (%08x %08x ...): a lab bag copy (packcopy) not undone? "
               "the teardown frees them twice and the next weapon created crashes (wp6 exe+2c9700, stage6 #87)", b0, b1, n, sh[0], n > 1 ? sh[1] : 0u);
}

/* the campaign difficulty: the property mCampaignDifficulty (registered at 0x755533) reads [[0x116A7E4]+0x30] (getter 0x746DF0); -1 = not readable / out of range */
#define RANKMAN_PTR 0x116A7E4u
static int campaign_diff(void)
{
    unsigned rm = rd32(RANKMAN_PTR);
    if (!rm) return -1;
    unsigned d = rd32(rm + 0x30);
    return d <= RB_DIFF_MAX ? (int)d : -1;
}
/* the guest takes the host's difficulty right before its engine call (its own save's value would set enemies and damage otherwise) */
static void take_host_diff(int d)
{
    unsigned rm = rd32(RANKMAN_PTR);
    int own = campaign_diff();
    if (d < 0 || !rm || own == d) return;
    uint32_t v = (uint32_t)d;
    if (m_write(NULL, rm + 0x30, &v, 4)) r1_log("barrier: the guest takes the host's difficulty: %d -> %d", own, d);
    else r1_log("barrier: the host's difficulty %d could not be written (own %d)", d, own);
}

static int running_area(char *name, int size, unsigned *hash);

/* continue1 (A): the campaign checkpoint record of sCheckPoint ([0x116A334] + 0x60 + mode * 0x24, mode = [[[0x1169108]+0x24]+0x20], as menu.c reads it); 0 = not there */
static unsigned cp_rec_addr(void)
{
    unsigned cp = rd32(CHECKPT_PTR), g = rd32(GAMESYS_PTR), o = g ? rd32(g + 0x24) : 0;
    if (!cp || !o) return 0;
    unsigned mode = rd32(o + 0x20);
    return mode <= 1 ? cp + 0x60 + mode * 0x24 : 0;
}
static int cp_read(rb_cp *out)
{
    unsigned r = cp_rec_addr();
    out->have = 0;
    if (!r) return 0;
    out->have = 1, out->id = (int32_t)rd32(r + 0x18), out->mask = rd32(r + 0x10);
    return 1;
}
static int wr_data(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, p, n, &got) && got == n;
}
/* the guest's record takes the host's checkpoint before its engine reloads (the area start's movie decision FUN_0041c8d0 reads +0x18) */
static void cp_take(const rb_triple *t, const rb_cp *cp)
{
    unsigned r = cp_rec_addr();
    uint32_t w[9];
    if (!r || !r1_safe_read((void *)(uintptr_t)r, w, sizeof w)) {
        r1_log("barrier: the host's checkpoint record (mID %d) could NOT be taken: this PC's record is not readable", cp->id);
        return;
    }
    uint32_t old[9];
    memcpy(old, w, sizeof w);
    if (!rb_cp_rec_take(w, t, cp)) {
        r1_log("barrier: this PC's checkpoint record already is the host's (mID %d, mask %04x, area %08x %d / %d)", (int32_t)w[6], w[4] & 0xFFFFu, w[5], (int32_t)w[7], (int32_t)w[8]);
        return;
    }
    int ok = wr_data(r + 8, &w[2], 4) && wr_data(r + 0x10, &w[4], 20);
    rb_cprec m = {1, (int32_t)w[6], w[4], w[5], (int32_t)w[7], (int32_t)w[8]};
    EnterCriticalSection(&cs);
    rb_cpm_mark(&B, &m);   /* task 4: the mod's own write, not a pass of this PC */
    LeaveCriticalSection(&cs);
    r1_log("barrier: this PC's checkpoint record takes the host's%s: mID %d -> %d, mask %04x -> %04x, area %08x -> %08x, %d / %d -> %d / %d (the reload %s the section's opening movie)",
           ok ? "" : " (WRITE FAILED)", (int32_t)old[6], (int32_t)w[6], old[4] & 0xFFFFu, w[4] & 0xFFFFu, old[5], w[5], (int32_t)old[7], (int32_t)old[8], (int32_t)w[7], (int32_t)w[8],
           rb_start_movie_plays(0, 1, (int32_t)w[6]) ? "plays" : "skips");
}

/* continue1 task 4: the whole record (the checkpoint mirror compares it) */
static int cp_read_full(rb_cprec *o)
{
    unsigned r = cp_rec_addr();
    memset(o, 0, sizeof *o);
    if (!r) return 0;
    uint32_t w[9];
    if (!r1_safe_read((void *)(uintptr_t)r, w, sizeof w)) return 0;
    o->have = 1, o->mask = w[4], o->area = w[5], o->id = (int32_t)w[6], o->a = (int32_t)w[7], o->b = (int32_t)w[8];
    return 1;
}
/* the engine's own checkpoint pass, as FUN_006d1e90 makes it at a level start: a cZoneContentsCheckPoint (ctor FUN_008d5400: mID -1, scenario no / flag -1, mFlag 0)
 * with the partner's mID and checkpoints, FUN_006d1a70(sCheckPoint, content) (thiscall, ret 4: the record, then the checkpoint save of this PC's own human), the dtor
 * FUN_008d4f40. Every function's first bytes are checked; game thread (the frame). 1 = called. */
#define CP_PASS_FN 0x6D1A70u
#define CP_CTOR_FN 0x8D5400u
#define CP_DTOR_FN 0x8D4F40u
static int cp_pass(const rb_cprec *r, char *why, int wsz)
{
    static const unsigned char pass_sig[12] = {0x51, 0xA1, 0x08, 0x91, 0x16, 0x01, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xE9};
    static const unsigned char ctor_sig[5] = {0x56, 0x57, 0x8B, 0xF1, 0xE8}, dtor_sig[6] = {0xC7, 0x01, 0xCC, 0xC1, 0x09, 0x01};
    unsigned char b[12];
    if (!r1_safe_read((void *)(uintptr_t)CP_PASS_FN, b, 12) || memcmp(b, pass_sig, 12)) return snprintf(why, (size_t)wsz, "FUN_006d1a70's bytes differ"), 0;
    if (!r1_safe_read((void *)(uintptr_t)CP_CTOR_FN, b, 5) || memcmp(b, ctor_sig, 5)) return snprintf(why, (size_t)wsz, "FUN_008d5400's bytes differ"), 0;
    if (!r1_safe_read((void *)(uintptr_t)CP_DTOR_FN, b, 6) || memcmp(b, dtor_sig, 6)) return snprintf(why, (size_t)wsz, "FUN_008d4f40's bytes differ"), 0;
    unsigned cp = rd32(CHECKPT_PTR);
    if (!cp) return snprintf(why, (size_t)wsz, "no sCheckPoint"), 0;
    static unsigned char content[0x60] __attribute__((aligned(16)));
    memset(content, 0, sizeof content);
    ((void(__attribute__((fastcall)) *)(void *))(uintptr_t)CP_CTOR_FN)(content);
    int16_t id = (int16_t)r->id, a = (int16_t)r->a, bb = (int16_t)r->b;
    memcpy(content + 0x10, &id, 2), memcpy(content + 0x1C, &a, 2), memcpy(content + 0x1E, &bb, 2);
    ((void(TC *)(unsigned, void *))(uintptr_t)CP_PASS_FN)(cp, content);
    ((void(__attribute__((fastcall)) *)(void *))(uintptr_t)CP_DTOR_FN)(content);
    snprintf(why, (size_t)wsz, "called");
    return 1;
}

static const char *restart_caller(uint32_t ret)
{
    switch (ret) {
    case 0x6D0E64u: return "the stock Continue (FUN_006d0cb0 after the checkpoint read)";
    case 0x6D1217u: case 0x6D1292u: return "the title Continue (FUN_006d0ff0)";
    case 0x6D14D8u: return "FUN_006d1470";
    default: return "the mod / another caller";
    }
}

static uint32_t __attribute__((force_align_arg_pointer)) cb_restart(rbk_frame *f)
{
    rb_triple t = {f->arg[0], (int32_t)f->arg[1], (int32_t)f->arg[2]};
    uint32_t hold = 0;
    char what[220];
    int diff_take = -1;
    rb_cp cp_take_it = {0, 0, 0};
    rb_triple cp_t = t;
    EnterCriticalSection(&cs);
    if (B.host) {
        B.tx_diff = campaign_diff();
        cp_read(&B.tx_cp);   /* continue1: the host's record after its own checkpoint read (the stock Continue) travels with RESTART */
        if (rb_restart_host(&B, &t) == RB_ST_EP1) g_ep_stop = 1;
        snprintf(what, sizeof what, "restart as the host: area %08x checkpoints %d / %d (%s)", t.area, t.a, t.b, B.why);
        mark(RBK_RESTART, 0, what);
    } else {
        rb_triple own = t;
        B.call_stock = f->ret_addr == 0x6D0E64u;   /* continue1: only the stock Continue's call may be dropped / held */
        int r = rb_restart_client(&B, &t);
        B.call_stock = 0;
        int changed = (r == 1 && (t.area != own.area || t.a != own.a || t.b != own.b)) || r == -1;
        if (r == 1) f->arg[0] = t.area, f->arg[1] = (uint32_t)t.a, f->arg[2] = (uint32_t)t.b, diff_take = B.applied_diff, cp_take_it = B.applied_cp, cp_t = t;
        hold = r == -1;
        snprintf(what, sizeof what, "restart as the client or offline: own area %08x %d / %d -> %s area %08x %d / %d (%s)", own.area, own.a, own.b, hold ? "NOT NOW" : "runs with", t.area, t.a, t.b, B.why);
        mark(RBK_RESTART, changed, what);
    }
    r1_log("barrier: restart #%u from %08x (%s): %s", HS[RBK_RESTART].calls, f->ret_addr, restart_caller(f->ret_addr), what);   /* rare: every call is logged */
    LeaveCriticalSection(&cs);
    if (!hold) {   /* the engine reloads right after this returns */
        if (diff_take >= 0) take_host_diff(diff_take);
        if (cp_take_it.have) cp_take(&cp_t, &cp_take_it);
        before_restart();
    }
    return hold;
}

/* continue1 (B): the aCut area's load step FUN_0041cfc0 (POST): "ready" (al 1) becomes "not ready" while this PC's restarted level waits for the other PC's */
static uint32_t __attribute__((force_align_arg_pointer)) cb_lstart(uint32_t self, uint32_t result)
{
    if (!(result & 0xFFu)) return result;
    EnterCriticalSection(&cs);
    int hold = rb_lstart_gate(&B, 1) == RB_HOLD;
    mark(RBK_LSTART, hold, hold ? "the level start waits for the other PC's (after a restart)" : "the level starts");
    LeaveCriticalSection(&cs);
    (void)self;
    return hold ? (result & ~0xFFu) : result;
}

/* continue1 (D): the stock Continue request FUN_0041efe0(byte) (PRE gate, plain ret) */
static uint32_t __attribute__((force_align_arg_pointer)) cb_cont(rbk_frame *f)
{
    int ours = g_cont_ours;
    EnterCriticalSection(&cs);
    int drop = rb_cont_request(&B, ours) == RB_HOLD;
    char what[160];
    snprintf(what, sizeof what, "%s: FUN_0041efe0(%u) from %08x (%s)", drop ? "DROPPED" : "goes on", f->arg[0] & 0xFFu, f->ret_addr,
             ours ? "the mod" : f->ret_addr == CONT_RET_STOCK1 || f->ret_addr == CONT_RET_STOCK2 ? "the area's request handler: game-over Yes / pause menu" : "another caller");
    mark(RBK_CONT, drop, what);
    r1_log("barrier: cont #%u: %s", HS[RBK_CONT].calls, what);
    LeaveCriticalSection(&cs);
    return (uint32_t)drop;
}

/* continue1 (C) / (D): the engine state in which the stock Continue may be asked (what FUN_006d0ff0 needs, else aStartContinue asserts); NULL = it may */
static const char *stock_continue_blocker(void)
{
    char an[48];
    unsigned h = 0, sm = rd32(SAVEMAN_PTR), cp = rd32(CHECKPT_PTR);
    if (!running_area(an, sizeof an, &h)) return "the running area is not readable";
    if ((h >> 24) != 0x92u && !(B.cfg.title_stock && !strcmp(an, "aRevelationsTitle")))   /* rev1 live 14: the title too with titlestock (LAB) */
        return "the running area is not a level (aCut)";
    if (!sm || !cp) return "no save manager / checkpoint system";
    if (rd32(sm + 0xB4)) return "the save manager is busy";
    if (rd32(sm + 0xE0) == 0xFFFFFFFFu) return "no save cache slot";
    if (rd32(cp + 0x5C) & 1u) return "a checkpoint read is pending";
    return NULL;
}
/* the mod asks this PC's stock Continue (outside the lock: the call goes through cb_cont) */
static int ask_stock_continue(void)
{
    char live[96];
    if (rbk_probe(&rbk_defs[RBK_CONT], &MEM, HS[RBK_CONT].block, live, sizeof live) == 0) return 0;
    g_cont_ours = 1;
    ((void (*)(unsigned))(uintptr_t)rbk_defs[RBK_CONT].va)(0);
    g_cont_ours = 0;
    return 1;
}
/* the mod's own call of FUN_00720870 (the follow, its retries, the held Continue, the console): through the restart hook (never dropped / held: not the stock caller) */
static unsigned call_restart(const rb_triple *t, const char *why)
{
    unsigned self = rd32(EPISODE_PTR);
    char live[96];
    int st = rbk_probe(&rbk_defs[RBK_RESTART], &MEM, HS[RBK_RESTART].block, live, sizeof live);
    if (!self || st == 0) {
        r1_log("barrier: %s: FUN_00720870 NOT called (episode object %08X, restart bytes %s)", why, self, live);
        return 0x100u;
    }
    unsigned r = ((unsigned char(TC *)(unsigned, unsigned, unsigned, unsigned))(uintptr_t)rbk_defs[RBK_RESTART].va)(self, t->area, (unsigned)t->a, (unsigned)t->b);
    r1_log("barrier: %s: FUN_00720870(%08X, %08X, %d, %d) returned %u", why, self, t->area, t->a, t->b, r & 0xFF);
    return r & 0xFFu;
}

/* rev1 live 15 (m14d / m14h / m14j, LIVE: after the host's TITLE Continue the guest's own human had NO weapon): the guest's own save read at the title, the way
 * the title menu's Continue does it (FUN_0083CC60 state 0x22, item 0x17 = Continue, 0x83DCAE..0x83DD03): with sSaveManager +0x5C0 == 1 (the HD save flow)
 * FUN_008B5D20(sm, 0) = the slot 0 entry, FUN_0081F940(entry) = usable, then FUN_008B3430(uSaveFlowHD = [the title menu manager + 0x68], 0) queues the
 * slot read (its job FUN_008B2340: FUN_008B9260(sm, [0x11469A8], 0, 6)); the menu's state 99 waits for the runner's queue (FUN_008B1550(runner + 0x30))
 * and reads the result at runner + 0x184, then FUN_006D0FF0 -> FUN_00720870. Without +0x5C0: the cache read FUN_008B9260(sm, [0x11436FC], [sm + 0xE0], 1).
 * The level start then takes the bag from sItemBox (FUN_005C8CC0 -> vf+0x48(the character)). The mod calls FUN_00720870 with the host's triple after it. */
#define TL_SLOT_ENTRY_VA 0x8B5D20u   /* thiscall(sm, slot) -> entry, ret 4 */
#define TL_ENTRY_OK_VA 0x81F940u     /* fastcall(entry) -> al */
#define TL_SLOT_READ_VA 0x8B3430u    /* thiscall(uSaveFlowHD, slot), ret 4 */
#define TL_CACHE_READ_VA 0x8B9260u   /* thiscall(sm, flags, slot, mode), ret 0xC */
#define TL_CACHE_FLAGS_PTR 0x11436FCu
#define TL_MGR_VTABLE 0x108CA30u     /* uTitleMenuManager */
static const unsigned char TL_B_ENTRY[] = {0x51, 0x8B, 0x44, 0x24, 0x08, 0x50, 0x8D, 0x54};
static const unsigned char TL_B_OK[] = {0x83, 0x79, 0x30, 0x00, 0x75, 0x0C, 0x83, 0x79};
static const unsigned char TL_B_SLOT[] = {0xA1, 0x28, 0xA3, 0x16, 0x01, 0x81, 0xEC, 0x80};
static const unsigned char TL_B_CACHE[] = {0x8B, 0x44, 0x24, 0x04, 0xC7, 0x81, 0xEC, 0x00};
static int g_tl_mode;                /* 1 = the slot read on the title's uSaveFlowHD, 2 = the cache read on sSaveManager */
static unsigned g_tl_runner, g_tl_sm;
static int code_is(unsigned va, const unsigned char *want, unsigned n)
{
    unsigned char c[16];
    return n <= sizeof c && r1_safe_read((void *)(uintptr_t)va, c, n) && !memcmp(c, want, n);
}
static unsigned rd8u(unsigned a)
{
    unsigned char v = 0;
    r1_safe_read((void *)(size_t)a, &v, 1);
    return v;
}
/* game thread, outside the lock: NULL = the read was asked */
static const char *title_read_ask(void)
{
    unsigned sm = rd32(SAVEMAN_PTR);
    g_tl_mode = 0, g_tl_runner = 0, g_tl_sm = sm;
    if (!sm) return "no save manager";
    if (rd32(sm + 0xB4)) return "the save manager is busy";
    unsigned hd = rd8u(sm + 0x5C0), cslot = rd32(sm + 0xE0);
    if (hd == 1) {
        unsigned mgr = menu_title_mgr(), runner = mgr ? rd32(mgr + 0x68) : 0;
        if (!mgr || rd32(mgr) != TL_MGR_VTABLE) return "no title menu manager";
        if (coopsave_active() && !coopsave_file_exists()) return "this PC has no CO-OP save yet (a fresh co-op start: the direct follow)";   /* task 5 */
        if (!runner) return "the title menu manager has no save flow (+0x68)";
        /* (task 5: coopsave1_game.c may have put its jump over the first 5 bytes of FUN_008B5D20: the bytes after it are checked then) */
        int entry_ok = code_is(TL_SLOT_ENTRY_VA, TL_B_ENTRY, sizeof TL_B_ENTRY) || (rd8u(TL_SLOT_ENTRY_VA) == 0xE9 && code_is(TL_SLOT_ENTRY_VA + 5, TL_B_ENTRY + 5, 3));
        if (!entry_ok || !code_is(TL_ENTRY_OK_VA, TL_B_OK, sizeof TL_B_OK) || !code_is(TL_SLOT_READ_VA, TL_B_SLOT, sizeof TL_B_SLOT))
            return "the save read code differs from this build";
        unsigned e = ((unsigned(TC *)(unsigned, unsigned))(uintptr_t)TL_SLOT_ENTRY_VA)(sm, 0);
        if (!e) return coopsave_active() ? "this PC has no CO-OP save yet (slot 0 empty in the co-op save)" : "this PC has no Continue save (slot 0 empty)";
        unsigned usable = ((unsigned(__attribute__((fastcall)) *)(unsigned))(uintptr_t)TL_ENTRY_OK_VA)(e) & 0xFFu;
        if (!usable) return "this PC's slot 0 save is not usable";
        r1_log("barrier: title follow: this PC's own save read asked: FUN_008b3430(save flow %08X, slot 0) (sSaveManager %08X, HD flag 1, cache slot %d, slot 0 entry %08X)", runner, sm,
               (int)cslot, e);
        ((void(TC *)(unsigned, unsigned))(uintptr_t)TL_SLOT_READ_VA)(runner, 0);
        g_tl_mode = 1, g_tl_runner = runner;
        return NULL;
    }
    if (cslot == 0xFFFFFFFFu) return "no save cache slot (HD flag 0)";
    if (!code_is(TL_CACHE_READ_VA, TL_B_CACHE, sizeof TL_B_CACHE)) return "the cache read code differs from this build";
    unsigned flags = rd32(TL_CACHE_FLAGS_PTR);
    r1_log("barrier: title follow: this PC's own save read asked: FUN_008b9260(sSaveManager %08X, %08X, cache slot %d, 1)", sm, flags, (int)cslot);
    ((void(TC *)(unsigned, unsigned, unsigned, unsigned))(uintptr_t)TL_CACHE_READ_VA)(sm, flags, cslot, 1);
    g_tl_mode = 2;
    return NULL;
}
/* the read is over (FUN_008B1550(runner + 0x30) = 1: no job, the ring of uSaveFlowHD + 0x34 empty; and the save manager idle); *ok = runner + 0x184 */
static int title_read_done(int *ok)
{
    unsigned sm = g_tl_sm;
    *ok = 0;
    if (g_tl_mode == 1 && g_tl_runner) {
        unsigned q = g_tl_runner + 0x30, cur = rd32(q + 0x12C), head = rd32(q + 0x128), tail = rd32(q + 0x124);
        int n = head < tail ? (int)(head - tail) + 9 : (int)(head - tail);
        if (cur || n || (sm && rd32(sm + 0xB4))) return 0;
        *ok = rd8u(g_tl_runner + 0x184) != 0;
        return 1;
    }
    if (g_tl_mode == 2) {
        if (sm && rd32(sm + 0xB4)) return 0;
        *ok = 1;
        return 1;
    }
    return 1;
}

int rolebarrier_shielded(void) { return g_shield; }
void rolebarrier_shield_drop(int pchit)
{
    unsigned n = ++g_shield_drops[pchit ? 1 : 0];
    if (n <= 8 || (n & (n - 1)) == 0) r1_log("barrier: shield: an enemy's blow on this PC's own human dropped (%s, #%u): the barrier after the restart has not opened yet", pchit ? "PCHIT" : "ApplyHit", n);
}

/* the running area: sGameArea [0x11E01E8], current = [+0x3828 + 4 * [+0x3824]] (menu.c area_name, live lm3); its class name and the DTI hash (the getter in
 * vtable slot 4 is `mov eax, DTI; ret`, the hash at DTI+0x1C: cEpisodeEndState_vf7 reads the finished section's id the same way, 0x42D3CB). 1 = readable. */
#define AREA_MGR_PTR 0x11E01E8u
static int running_area(char *name, int size, unsigned *hash)
{
    unsigned ga = rd32(AREA_MGR_PTR), idx = ga ? rd32(ga + 0x3824) : 0, area = idx ? rd32(ga + 0x3828 + 4 * idx) : 0;
    if (name && size) name[0] = 0;
    if (hash) *hash = 0;
    if (!area || (name && !classes_object_name(area, name, size))) return 0;
    if (hash) {
        unsigned vt = rd32(area), fn = vt ? rd32(vt + 16) : 0;
        unsigned char c[6] = {0};
        if (fn > 0x10000 && r1_safe_read((void *)(uintptr_t)fn, c, 6) && c[0] == 0xB8 && c[5] == 0xC3) {
            unsigned dti;
            memcpy(&dti, c + 1, 4);
            *hash = rd32(dti + 0x1C);
        }
    }
    return 1;
}

/* FUN_00720540(this, record, area): the start record is filled from the start table. The host sends it in the next frame (the record is complete then) unless the
 * caller is the section change (cEpisodeEndState); observer only, the function always runs. ep1-flow: the section change is this PC's own (rb_section_change): the
 * host sends the next section's start triple (E4); out of Episode 1 = the end of Episode 1 on this PC (E5). */
static uint32_t __attribute__((force_align_arg_pointer)) cb_start(rbk_frame *f)
{
    int section = f->ret_addr >= RB_SECTION_FN_LO && f->ret_addr < RB_SECTION_FN_HI;
    unsigned from = 0;
    char an[48] = "";
    rb_triple nx;
    int nx_ok = 0;
    if (section) {   /* memory reads outside the lock */
        unsigned run = 0;
        running_area(an, sizeof an, &run);
        from = sec_of(g_vf7_from) ? sec_of(g_vf7_from) : sec_of(run);   /* what vf7 itself read, else the running area */
        nx_ok = rb_table_triple(rd32_u, NULL, GAMESYS_PTR, f->arg[1], &nx);
    }
    EnterCriticalSection(&cs);
    int st = rb_start_seen(&B, f->ret_addr);
    if (st) g_st_area = f->arg[1];
    mark(RBK_START, 0, "observer");
    if (g_logged[RBK_START] < 40) {
        g_logged[RBK_START]++;
        r1_log("barrier: start #%u: FUN_00720540(record %08x, area %08x) from %08x%s", HS[RBK_START].calls, f->arg[0], f->arg[1], f->ret_addr,
               st ? ": the host's story start, sent next frame" : section ? " (section change)" : " (not a host start)");
    }
    if (section) {
        r1_log("barrier: section change on this PC (%s): finished section %08x (running area %s) -> %08x, start table %s %d / %d", B.host ? "host" : "client", from, an[0] ? an : "?",
               f->arg[1], nx_ok ? "entry" : "has NO entry", nx_ok ? nx.a : 0, nx_ok ? nx.b : 0);
        int r = rb_section_change(&B, from, f->arg[1], nx_ok ? &nx : NULL);
        if (r == RB_SC_EPEND) {
            g_ep1_local = 1;
            if (!g_ep1_t) g_ep1_t = GetTickCount() | 1;
        }
    }
    LeaveCriticalSection(&cs);
    return 0;
}

/* the start record [[[0x1169108]+0x24]+4] */
static int start_record(rb_triple *t)
{
    unsigned g = rd32(GAMESYS_PTR), o = g ? rd32(g + 0x24) : 0, rec = o ? rd32(o + 4) : 0;
    if (!rec) return 0;
    t->area = rd32(rec + 4), t->a = (int32_t)rd32(rec + 8), t->b = (int32_t)rd32(rec + 0xC);
    return 1;
}

static uint32_t rd32_cb(void *user, uint32_t a)
{
    (void)user;
    return rd32(a);
}

/* the start triple of the pending story start: the record when it still holds a start (the title Continue / the checkpoint reload: 720870 wrote the checkpoint
 * there, the restart hook sent it: a duplicate), else the start table's entry of FUN_00720540's area argument (lm2 live: after the title's New Game the record
 * held no start a frame later) */
static int start_triple_now(rb_triple *t)
{
    rb_triple r;
    int have = start_record(&r);
    if (have && rb_triple_ok(&r, NULL, 0)) {
        *t = r;
        return 1;
    }
    if (rb_table_triple(rd32_cb, NULL, GAMESYS_PTR, g_st_area, t)) {
        r1_log("barrier: host story start: the start record holds %s (area %08x %d / %d): the start table's entry of area %08x is used (area %08x %d / %d)", have ? "no start" : "nothing",
               have ? r.area : 0u, have ? r.a : 0, have ? r.b : 0, g_st_area, t->area, t->a, t->b);
        return 1;
    }
    if (have) *t = r;
    r1_log("barrier: host story start: area %08x is not in the start table", g_st_area);
    return have;
}

/* a level of this PC: the sPlayer list has a player unit (the title has none) */
static int level_present(void)
{
    unsigned sp = rd32(R1_SPLAYER_PTR);
    if (!sp) return 0;
    unsigned head = rd32(sp + 0x30), endp = rd32(sp + 0x34);
    for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8)) {
        unsigned unit = rd32(head + 0x10);
        if (unit && is_player_class(unit)) return 1;
    }
    return 0;
}

unsigned rolebarrier_ep_block_ms(void) { return g_ep_block_ms; }
int rolebarrier_level_present(void) { return level_present(); }   /* rev1 live 15: coopsave1_game.c */
static int running_area(char *name, int size, unsigned *hash);
/* rev1 live 15 (m15l9): the running area is the title (aRevelationsTitle: the press-start screen and the title menu) */
int rolebarrier_title_area(void)
{
    char an[48];
    return running_area(an, sizeof an, NULL) && !strcmp(an, "aRevelationsTitle");
}

/* rev1 live 10: the section-end game flags (rolebarrier1.h RB_GF_*), set or cleared the way FsmSetGameFlag 0x747880 does it: FUN_0078EB80([0x1169110], type)
 * finds the flag object of the type (a plain walk of [holder+0x34][0..[holder+0x28]) comparing each object's DTI hash), then its vf+0x14 (set) / vf+0x18
 * (clear) with the bit. The bytes of both functions are checked first; the slot must point into the exe's code. Game thread. 1 = done (read back by vf+0x24). */
#define GF_HOLDER_PTR 0x1169110u
#define GF_LOOKUP_VA 0x78EB80u
#define GF_SETFLAG_VA 0x747880u
static const uint8_t gf_lookup_bytes[16] = {0x53, 0x57, 0x8b, 0xd9, 0x33, 0xc0, 0x33, 0xff, 0x39, 0x43, 0x28, 0x76, 0x2d, 0x55, 0x8b, 0x6c};
static const uint8_t gf_setflag_bytes[12] = {0xa1, 0x10, 0x91, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14, 0x68, 0xf8, 0xb0};
static int gf_code_ptr(unsigned p) { return p >= 0x401000u && p < 0xF00000u; }
static int game_flag_bit(unsigned type, unsigned bit, int set, char *why, size_t wsz)
{
    uint8_t got[16];
    unsigned holder = rd32(GF_HOLDER_PTR);
    if (!r1_safe_read((void *)(uintptr_t)GF_LOOKUP_VA, got, sizeof gf_lookup_bytes) || memcmp(got, gf_lookup_bytes, sizeof gf_lookup_bytes) ||
        !r1_safe_read((void *)(uintptr_t)GF_SETFLAG_VA, got, sizeof gf_setflag_bytes) || memcmp(got, gf_setflag_bytes, sizeof gf_setflag_bytes)) {
        snprintf(why, wsz, "FUN_0078EB80 / FsmSetGameFlag bytes differ (another build?)");
        return 0;
    }
    if (!holder) { snprintf(why, wsz, "no game flag holder [0x1169110]"); return 0; }
    unsigned obj = ((unsigned(TC *)(unsigned, unsigned))(uintptr_t)GF_LOOKUP_VA)(holder, type);
    unsigned vt = obj ? rd32(obj) : 0, fset = vt ? rd32(vt + (set ? 0x14u : 0x18u)) : 0, ftest = vt ? rd32(vt + 0x24u) : 0;
    if (!obj || !gf_code_ptr(fset) || !gf_code_ptr(ftest)) {
        snprintf(why, wsz, "type %u: object %08X vtable %08X slots %08X / %08X not usable", type, obj, vt, fset, ftest);
        return 0;
    }
    ((void(TC *)(unsigned, unsigned))(uintptr_t)fset)(obj, bit);
    unsigned now = ((unsigned char(TC *)(unsigned, unsigned))(uintptr_t)ftest)(obj, bit) & 0xFFu;
    snprintf(why, wsz, "type %u object %08X bit %u %s: reads %u", type, obj, bit, set ? "set" : "cleared", now);
    return set ? now != 0 : now == 0;
}
static int section_end_flags(int set)   /* both section-end flags as the final script sets them (set) or both cleared */
{
    char w1[120], w2[120];
    int a = game_flag_bit(RB_GF_TYPE_EPISODE, RB_GF_SECTION_END, set, w1, sizeof w1), c = game_flag_bit(RB_GF_TYPE_CUT, RB_GF_SECTION_END, set, w2, sizeof w2);
    r1_log("barrier: the section-end game flags %s on this PC: %s; %s%s", set ? "SET (this PC's own stock section end comes)" : "cleared", w1, w2, a && c ? "" : " - NOT both");
    return a && c;
}

unsigned rolebarrier_ep1_end(int *local, unsigned *closed)
{
    unsigned t = g_ep1_t;
    if (t && GetTickCount() - t > 2400000u) t = 0;   /* 40 minutes (> the 30 min close cap): history */
    if (local) *local = t ? g_ep1_local : 0;
    if (closed) *closed = t ? g_ep1_closed : 0;
    return t;
}

static const uint32_t CBS_DUMMY = 0;
static uint32_t cb_addr(int i)
{
    switch (i) {
    case RBK_EPEND: return (uint32_t)(uintptr_t)cb_epend;
    case RBK_PCS: return (uint32_t)(uintptr_t)cb_pcs;
    case RBK_MOVIE: return (uint32_t)(uintptr_t)cb_movie;
    case RBK_PCSEND: return (uint32_t)(uintptr_t)cb_pcsend;
    case RBK_RESTART: return (uint32_t)(uintptr_t)cb_restart;
    case RBK_START: return (uint32_t)(uintptr_t)cb_start;
    case RBK_LSTART: return (uint32_t)(uintptr_t)cb_lstart;
    case RBK_CONT: return (uint32_t)(uintptr_t)cb_cont;
    default: return CBS_DUMMY;
    }
}

/* the hooks follow `want` (session / REV1_BARRIER / `barrier apply`), each overridden by its own console mode; only a CHANGE of the wish writes or reverts (a refusal is not retried every frame) */
static void hooks_sync(int want)
{
    for (int i = 0; i < RBK_N; i++) {
        int d = hm[i] == 1 ? 1 : hm[i] == 2 ? 0 : (want && !skipped[i]);
        if (d == hook_last[i]) continue;
        if (d) rbk_apply(&HS[i], &rbk_defs[i], &MEM, cb_addr(i));
        else rbk_revert(&HS[i], &rbk_defs[i], &MEM);
        if (!d) changing[i] = 0;
        hook_last[i] = d;
    }
}

/* ---------------------------------------------------------------- one pass per logic frame */
static void apply_role(int session_ok, int role)
{
    rb_role_act a;
    if (rb_role_step(&R, session_ok, role, &a) == RBR_APPLY) {
        if (a.prev >= 0 && a.prev != a.role) {   /* rev1 live 16: the roles of the co-op save put back at the host's title Continue: the old role's capture goes first */
            capture_unbind(a.prev);
            r1_log("barrier: the role changed in the started session (%s -> %s, the co-op save's roles): capture_unbind(%d)", a.prev ? "partner" : "lead", a.role ? "partner" : "lead", a.prev);
        }
        capture_set_own(a.own_req);
        capture_bind(a.role, 0);   /* this PC's human plays its own character on pad 0 (the partner is the puppet's business: puppet_game.c binds it when `net pad on`) */
        r1_log("barrier: role applied: this PC plays the %s (unit slot %d): capture_set_own(%d), capture_bind(%d, 0)", a.role ? "partner" : "lead", a.role, a.own_req, a.role);
    } else if (a.act == RBR_RELEASE) {
        capture_set_own(-1);
        capture_unbind(a.role);
        r1_log("barrier: role released (session ended or `barrier role off`): capture_set_own(-1), capture_unbind(%d)", a.role);
    } else if (R.applied) {
        capture_state_t st;   /* the puppet module unbinds all roles when the session leaves PLAYING: keep ours */
        capture_get_state(&st);
        if (!st.role[R.role].enabled) capture_bind(R.role, 0), g_role_pad_checks++;
        if (R.role == 1 && !(st.own_on && st.own_slot == 1)) {   /* the partner's PC answers own slot 1 for the whole session (pickups, doors, prompts) */
            capture_set_own(1);
            if (g_role_own_fixes++ < 8) r1_log("barrier: the partner's own slot was lost (own %s, slot %u): capture_set_own(1) again (#%d)", st.own_on ? "on" : "off", st.own_slot, g_role_own_fixes);
        }
    }
}

static void barrier_frame(void)
{
    if (!inited) return;
    EnterCriticalSection(&cs);
    int active = net1_session_active();
    lk_t *k = net1_link_peek();
    lk_info in;
    memset(&in, 0, sizeof in);
    in.role = -1;
    if (k) {
        lk_get_info(k, &in);
        if (reg_link != k) {
            lk_add_event_cb(k, on_event, NULL);
            reg_link = k;
        }
    }
    int want = mode == 1 || (mode == 0 && (env_on || active));
    B.host = in.host != 0;
    if (active && !g_was_active) {   /* a new START: the last session's rendezvous, triples and end of Episode 1 are forgotten (with REV1_BARRIER=1 B stays enabled between sessions) */
        rb_reset(&B);
        g_ep1_t = 0, g_ep1_local = 0, g_ep1_closed = 0, g_ep1_close = 0;
    }
    g_was_active = active;
    rb_set_enabled(&B, want);
    apply_role(mode != 2 && role_mode != 2 && active, in.role);
    hooks_sync(want);
    hooks_last = want;
    if (want) {
        drain_events();
        if (B.ep_peer && !g_ep1_t) g_ep1_t = GetTickCount() | 1;   /* E5: the partner passed the end of Episode 1 */
        if (active && B.ep_local && !B.ep_closed) {   /* E5: this PC's own end: close the session after the stock end flow (result, save) */
            static char last_an[48];
            char an[48];
            unsigned h = 0;
            int rd = running_area(an, sizeof an, &h);
            /* the stock end flow: aEpisodeEnd (vf7's next class for the next episode: result, save states) and the flow areas it may pass through for the
             * result / the save (aSceneResult, aSceneSave: cFlowManageArea classes of work\dti-rev1.txt); anything else after it (aEpisodeTitle) = over */
            int in_end = !rd ? -1 : !strcmp(an, "aEpisodeEnd") || !strcmp(an, "aSceneSave") || !strcmp(an, "aSceneResult");
            int lvl_out = rd && level_present() && sec_of(h) && !rb_ep1_area(sec_of(h));   /* (aCut2100 = 2-1 loaded) */
            if (rd && strcmp(an, last_an)) {
                r1_log("barrier: END OF EPISODE 1: the running area is now %s (%08x)%s", an, h, lvl_out ? ", a level outside Episode 1" : "");
                snprintf(last_an, sizeof last_an, "%s", an);
            }
            if (rb_ep_close_step(&B, in_end, lvl_out, GetTickCount())) g_ep1_close = 1, snprintf(g_ep1_area, sizeof g_ep1_area, "%s", rd ? an : "?");
        }
        if (load_mode == 2) {
            /* the automatic load barrier is off (`barrier load off`): the session stays where it is until `net load` / `net done` */
        } else if (active && (in.role == 0 || in.role == 1)) {
            unsigned root = rd32(CAMROOT_PTR), mgr0 = root ? rd32(root + 0xCE0) : 0, own = unit_by_slot((unsigned)in.role);
            float pos[3] = {0, 0, 0};
            if (own) r1_safe_read((void *)(size_t)(own + 0x40), pos, sizeof pos);
            int playable = root && mgr0 && own && rb_placed(pos[0], pos[1], pos[2]);   /* e1w2: the units wait at 0,0,0 through the load / start movie */
            rb_load_step(&B, playable);
        } else
            rb_load_step(&B, 0);
    }
    /* the host's story start (a frame after FUN_00720540: the record is complete) and the Episode 1 gate */
    if (want && active && B.host && B.st_pending) {
        rb_triple rec;
        int ok = start_triple_now(&rec);
        B.tx_diff = campaign_diff();   /* New Game: the difficulty the player just chose (live check: the host's log names it) */
        int r = rb_start_triple(&B, ok ? &rec : NULL);
        if (r == RB_ST_EP1) g_ep_stop = 1;
        else if (r == RB_ST_DUP) r1_log("barrier: host story start: the restart hook sent this triple already (area %08x %d / %d)", rec.area, rec.a, rec.b);
    }
    int ep_stop = g_ep_stop, ep1_close = g_ep1_close;
    g_ep_stop = 0, g_ep1_close = 0;
    rb_triple fol;
    int at_title = !level_present();
    int folr = (want && active) ? rb_restart_follow_at(&B, &fol, at_title) : RB_FOL_NONE;   /* `barrier set follow 0` switches it off */
    int secend = folr == RB_FOL_SECEND;   /* rev1 live 10: the follow into the next section by this PC's own stock section end */
    int follow = folr == RB_FOL_CALL;
    if (follow) g_fol_last = fol, g_fol_t = GetTickCount(), g_fol_tries = 1;
    else if (want && active && !B.host && at_title && menu_title_age_ms() < 400 && g_fol_tries && g_fol_tries < 6 && GetTickCount() - g_fol_t > 3000 && B.tl_state != RB_TLS_ASK &&
             B.tl_state != RB_TLS_WAIT) {   /* (rev1 live 15: not while this PC's own save read runs) */   /* still on the title menu: the call did nothing */
        follow = 1, fol = g_fol_last, g_fol_t = GetTickCount(), g_fol_tries++;   /* the call at the title was refused (the save not read yet?): again, at most 6 times */
        r1_log("barrier: the client's story start did not load a level yet: try %u with the host's triple (area %08x %d / %d)", g_fol_tries, fol.area, fol.a, fol.b);
    }
    if (!at_title) g_fol_tries = 0;
    /* rev1 live 15: the guest reads its own save at the title before it follows a SAVE start (the read asked outside the lock; the call when it is over) */
    int tl_ask = 0;
    if (want && active && !B.host) {
        rb_triple tl_out;
        if (follow) {
            int g = rb_title_load_gate(&B, &fol, at_title);
            if (g == RB_TL_LOAD) follow = 0, tl_ask = 1;
            else if (g == RB_TL_WAIT) follow = 0;
        } else if (B.tl_state == RB_TLS_WAIT || !at_title) {
            int ok = 0, done = at_title ? title_read_done(&ok) : 0;
            if (rb_title_load_frame(&B, at_title, done, ok, &tl_out)) {
                follow = 1, fol = tl_out, g_fol_last = fol, g_fol_t = GetTickCount();
                if (!g_fol_tries) g_fol_tries = 1;
                unsigned cp = rd32(CHECKPT_PTR), g = rd32(GAMESYS_PTR), o = g ? rd32(g + 0x24) : 0, mode_ = o ? rd32(o + 0x20) : 0, rec = cp ? cp + 0x60 + mode_ * 0x24 : 0;
                r1_log("barrier: title follow: after this PC's save read: its checkpoint record area %08x mID %d flags %08x, sSaveManager cache slot %d, HD save flow result %u",
                       rec ? rd32(rec + 0x14) : 0, rec ? (int)rd32(rec + 0x18) : 0, rec ? rd32(rec + 8) : 0, g_tl_sm ? (int)rd32(g_tl_sm + 0xE0) : -2,
                       g_tl_runner ? rd8u(g_tl_runner + 0x184) : 0xFFu);
            }
        }
    }
    int try_stock = follow && B.fol_try_stock && (!at_title || B.cfg.title_stock);   /* rev1 live 14 (m14d): title_stock (LAB, default 0) */
    /* continue1: the shield (any thread reads g_shield), the guest's held own Continue, the host's answer to the guest's request */
    g_shield = (want && active) ? rb_shield_on(&B) : 0;
    rb_triple held_t;
    int held_host = 0, held_call = (want && active && !B.host) ? rb_held_step(&B, &held_t, &held_host) : 0;
    int cr_ask = (want && active && B.host) ? rb_cont_request_step(&B) : 0;
    /* continue1 task 4: the checkpoint pass mirror (this PC's new passes go out; the partner's is passed here the engine's own way) */
    rb_cprec cpo, cpa;
    int cp_call = 0;
    if (want && active) {
        char an[48];
        unsigned h = 0;
        cp_read_full(&cpo);
        int lvl = in.phase == 3 && level_present() && running_area(an, sizeof an, &h) && (h >> 24) == 0x92u;
        cp_call = rb_cpm_step(&B, &cpo, lvl, &cpa);
    }
    LeaveCriticalSection(&cs);
    if (cp_call) {
        char why[80];
        rb_cprec before = cpo, after;
        int called = cp_pass(&cpa, why, sizeof why);
        cp_read_full(&after);
        r1_log("barrier: the partner's checkpoint mID %d (%d / %d) passed here the engine's way: %s; record mID %d -> %d, mask %04x -> %04x, area %08x -> %08x", cpa.id, cpa.a, cpa.b, why,
               before.id, after.id, before.mask & 0xFFFFu, after.mask & 0xFFFFu, before.area, after.area);
        EnterCriticalSection(&cs);
        rb_cpm_applied(&B, called, &after);
        LeaveCriticalSection(&cs);
    }
    if (ep_stop) {   /* the host started outside Episode 1: the partner cannot follow, the session ends (the partner reads "the host has ended the session") */
        g_ep_block_ms = GetTickCount() | 1;
        r1_log("barrier: this PC started a part outside Episode 1: the network session is closed (Episode 1 only in this version)");
        net1_stop();
    }
    if (ep1_close) {   /* E5: after this PC's own stock end flow; the strip and the silence about the partner's end are menu.c's (rolebarrier_ep1_end) */
        g_ep1_closed = GetTickCount() | 1;
        r1_log("barrier: END OF EPISODE 1: this PC closes the network session (running area %s); Episode 2 is not played in the session", g_ep1_area);
        net1_stop();
    }
    if (secend) {   /* outside the lock: the flags the section's final script sets; cEpisodeEndState_vf7 follows within frames (epend / start hooks) */
        if (section_end_flags(1)) g_gf_set = 1, g_gf_sets++;
        else g_gf_fails++;   /* the restart call comes after RB_SECEND_GIVEUP_MS */
    }
    if (follow && g_gf_set) {   /* the fallback after a stock end that did not come: FUN_00720870's restart bit keeps the flags, the new level would end at once */
        section_end_flags(0);
        g_gf_set = 0, g_gf_clears++;
    }
    if (g_gf_set && !follow && !B.sec_pending) g_gf_set = 0;   /* (l10a) the own stock change consumed the SECTION: the flags were used, nothing to clear later */
    if (tl_ask) {   /* rev1 live 15: this PC's own save read at the title (the call comes in a later frame, rb_title_load_frame) */
        const char *why = title_read_ask();
        EnterCriticalSection(&cs);
        rb_title_load_asked(&B, why == NULL, why);
        LeaveCriticalSection(&cs);
        if (why) follow = 1, try_stock = 0;   /* not asked: the direct call now */
    }
    if (follow) {   /* outside the lock: the call goes through our own restart hook (cb_restart takes the lock and the triple) */
        /* continue1 (C): in a level the follow goes through this PC's own stock Continue (aStartContinue: its checkpoint read, the area start's movie decision with
         * the host's record, FUN_00720870 from FUN_006d0cb0 rewritten by the restart hook); the direct call when that cannot be asked or did not use the triple */
        const char *blk = try_stock ? stock_continue_blocker() : "";
        if (try_stock && !blk && ask_stock_continue()) {
            EnterCriticalSection(&cs);
            rb_follow_went(&B, 1);
            LeaveCriticalSection(&cs);
        } else {
            if (try_stock) r1_log("barrier: the follow cannot ask the stock Continue (%s): the direct call", blk ? blk : "its hook is not in force");
            EnterCriticalSection(&cs);
            rb_follow_went(&B, 0);
            LeaveCriticalSection(&cs);
            call_restart(&fol, "client follows the host's restart");
        }
    }
    if (held_call) call_restart(&held_t, held_host ? "this PC's own Continue loads the host's triple" : "this PC's own Continue loads its own checkpoint (no host triple came)");
    if (cr_ask) {   /* continue1 (D): the guest chose Continue: the host continues too (its own stock Continue; its restart hook sends RESTART) */
        const char *blk = stock_continue_blocker();
        if (!blk && ask_stock_continue()) {
            EnterCriticalSection(&cs);
            rb_cont_answered(&B, 1, "this PC's stock Continue was asked (FUN_0041efe0(0))");
            LeaveCriticalSection(&cs);
            g_cr_why = NULL;
        } else if (blk != g_cr_why) {
            g_cr_why = blk;
            r1_log("barrier: the guest's Continue request waits: %s", blk ? blk : "the Continue hook is not in force");
        }
    }
}

int rolebarrier_init(void)
{
    char v[200];
    if (inited) return 0;
    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&eq_cs);
    eq_ready = 1;
    rb_io io = {io_now, io_session, io_send, io_begin, io_done, io_log, NULL};
    rb_init(&B, &io, NULL);
    B.cfg.ep1_only = !(r1_env("REV1_ANYEPISODE", v, sizeof v) && v[0] == '1');   /* the product: Episode 1 only (lab: REV1_ANYEPISODE=1 lets 9-2 tests run) */
    rb_role_init(&R);
    env_on = r1_env("REV1_BARRIER", v, sizeof v) && atoi(v) == 1;
    if (r1_env("REV1_BARRIER_OFF", v, sizeof v)) {   /* comma separated hook names; "role" and "load" switch those parts off */
        for (char *tok = strtok(v, ", ;"); tok; tok = strtok(NULL, ", ;")) {
            for (int i = 0; i < RBK_N; i++)
                if (!strcmp(tok, rbk_defs[i].name)) skipped[i] = 1;
            if (!strcmp(tok, "role")) role_mode = 2;
            if (!strcmp(tok, "load")) load_mode = 2;
        }
    }
    inited = 1;
    lt_add_frame_cb(barrier_frame);
    r1_log("barrier: module ready (REV1_BARRIER=%d, role %s, load %s; the detours are written only in a network session, with REV1_BARRIER=1 or by `barrier apply`; the role of the joined player is set when the session starts)",
           env_on, role_mode == 2 ? "off" : "auto", load_mode == 2 ? "off" : "auto");
    return 0;
}

/* ---------------------------------------------------------------- console */
static int hook_by_name(const char *n)
{
    for (int i = 0; i < RBK_N; i++)
        if (!strcmp(n, rbk_defs[i].name)) return i;
    return -1;
}

static void status_text(char *out, int size)
{
    size_t n = 0;
    capture_state_t cst;
    capture_get_state(&cst);
    n += (size_t)snprintf(out + n, (size_t)size - n, "barrier: mode %s, env %d, session %d, hooks %s; role %s (%s%s, capture own %d, effective %d); load barrier %s\n",
                          mode == 0 ? "auto" : mode == 1 ? "ON" : "off", env_on, net1_session_active(), hooks_last ? "wanted" : "not wanted", role_mode == 2 ? "off" : "auto",
                          R.applied ? (R.role ? "partner" : "lead") : "none", R.applied ? " applied" : "", cst.own_on ? (int)cst.own_slot : -1, capture_own_effective(), load_mode == 2 ? "off" : "auto");
    for (int i = 0; i < RBK_N && n + 220 < (size_t)size; i++) {
        char live[96];
        rbk_probe(&rbk_defs[i], &MEM, HS[i].block, live, sizeof live);
        n += (size_t)snprintf(out + n, (size_t)size - n, "  %-7s %08X %-15s %-9s calls %u changed %u (episodes %u)%s%s%s%s\n", rbk_defs[i].name, rbk_defs[i].va, live, rbk_state_name(HS[i].st),
                              HS[i].calls, HS[i].changed, HS[i].episodes, hm[i] == 1 ? " [forced on]" : hm[i] == 2 ? " [forced off]" : "", skipped[i] ? " [left out]" : "", HS[i].why[0] ? " why: " : "",
                              HS[i].why);
    }
    char rs[3000];
    rb_status(&B, rs, sizeof rs);
    if (n < (size_t)size) n += (size_t)snprintf(out + n, (size_t)size - n, "  section-end game flags: set %u, failed %u, cleared before a fallback %u%s\n", g_gf_sets, g_gf_fails, g_gf_clears,
                                                 g_gf_set ? " (set now)" : "");
    if (n < (size_t)size) snprintf(out + n, (size_t)size - n, "  %s", rs);
}

void rolebarrier_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "", w1[24] = "", w2[24] = "", w3[24] = "", w4[24] = "", w5[24] = "";
    sscanf(arg, "%15s %23s %23s %23s %23s %23s", sub, w1, w2, w3, w4, w5);
    if (!inited) {
        snprintf(out, (size_t)size, "barrier: not initialised");
        return;
    }
    EnterCriticalSection(&cs);
    if (!sub[0] || !strcmp(sub, "status")) {
        status_text(out, size);
    } else if (!strcmp(sub, "apply") || !strcmp(sub, "revert")) {
        int on = sub[0] == 'a', which = w1[0] && strcmp(w1, "hooks") ? hook_by_name(w1) : -1;
        if (w1[0] && strcmp(w1, "hooks") && which < 0) {
            snprintf(out, (size_t)size, "barrier: no hook '%s' (epend|pcs|movie|pcsend|restart|start|lstart|cont|hooks)", w1);
        } else {
            if (w1[0]) {   /* one hook or all five; the role, the rendezvous and the load barrier stay as they are */
                for (int i = 0; i < RBK_N; i++)
                    if (which < 0 || i == which) hm[i] = on ? 1 : 2;
                r1_log("barrier: console: %s %s (hooks only)", sub, w1);
            } else {       /* everything */
                mode = on ? 1 : 2;
                memset(hm, 0, sizeof hm);
                r1_log("barrier: console: %s (everything: hooks, rendezvous, load barrier, role)", sub);
            }
            int want = mode == 1 || (mode == 0 && (env_on || net1_session_active()));
            rb_set_enabled(&B, want);
            hooks_sync(want);
            hooks_last = want;
            status_text(out, size);
        }
    } else if (!strcmp(sub, "role") || !strcmp(sub, "load")) {
        int *m = sub[0] == 'r' ? &role_mode : &load_mode;
        if (!strcmp(w1, "off")) *m = 2;
        else if (!strcmp(w1, "auto") || !strcmp(w1, "on")) *m = 0;
        r1_log("barrier: console: %s %s", sub, *m == 2 ? "off" : "auto");
        snprintf(out, (size_t)size, "barrier: %s %s (the next frame acts)%s", sub, *m == 2 ? "off" : "auto",
                 sub[0] == 'l' && *m == 2 ? "; the session load barrier now needs `net load` / `net done`" : "");
    } else if (!strcmp(sub, "auto")) {
        mode = 0, role_mode = 0, load_mode = 0;
        memset(hm, 0, sizeof hm);
        snprintf(out, (size_t)size, "barrier: auto (a network session or REV1_BARRIER=1 decides; the next frame acts)");
    } else if (!strcmp(sub, "set")) {
        unsigned v = (unsigned)strtoul(w2, NULL, 10);
        if (!strcmp(w1, "section")) B.cfg.section_ms = v;
        else if (!strcmp(w1, "pcs")) B.cfg.pcs_ms = v;
        else if (!strcmp(w1, "latch")) B.cfg.latch_ms = v;
        else if (!strcmp(w1, "notriple")) B.cfg.no_triple_hold_ms = (int)v;
        else if (!strcmp(w1, "follow")) B.cfg.follow_ms = (int)v;
        else if (!strcmp(w1, "ep1only")) B.cfg.ep1_only = v != 0;
        /* continue1: the switches (1 = the fix, 0 = the model before it) and the limits */
        else if (!strcmp(w1, "cpsync")) B.cfg.cp_sync = v != 0;
        else if (!strcmp(w1, "lhold")) B.cfg.lstart_hold = v != 0;
        else if (!strcmp(w1, "lstart")) B.cfg.lstart_ms = v;
        else if (!strcmp(w1, "shield")) B.cfg.shield = v != 0;
        else if (!strcmp(w1, "shieldms")) B.cfg.shield_ms = v;
        else if (!strcmp(w1, "stock")) B.cfg.follow_stock = v != 0;
        else if (!strcmp(w1, "titlestock")) B.cfg.title_stock = v != 0;
        else if (!strcmp(w1, "titleload")) B.cfg.title_load = v != 0;   /* rev1 live 15: the guest's own save read before the title follow (product default 1) */   /* rev1 live 14 (m14d): the title Continue's follow through the guest's own stock Continue */
        else if (!strcmp(w1, "stockwait")) B.cfg.stock_wait_ms = v;
        else if (!strcmp(w1, "contreq")) B.cfg.cont_req = v != 0;
        else if (!strcmp(w1, "contwait")) B.cfg.cont_wait_ms = v;
        else if (!strcmp(w1, "cpmirror")) B.cfg.cp_mirror = v != 0;
        else if (!strcmp(w1, "logs")) {   /* lab (r6): the capped hook log lines (pcs / movie / start: 40 each) count again from 0 */
            memset(g_logged, 0, sizeof g_logged);
            snprintf(out, (size_t)size, "barrier: hook log caps reset (the next 40 pcs / movie / start lines are logged)");
            LeaveCriticalSection(&cs);
            return;
        } else {
            snprintf(out, (size_t)size, "barrier set section|pcs|latch|notriple|follow|lstart|shieldms|stockwait|contwait <ms> | ep1only|cpsync|lhold|shield|stock|contreq 0|1 | logs 0");
            LeaveCriticalSection(&cs);
            return;
        }
        snprintf(out, (size_t)size, "barrier: section %u ms, cutscene %u ms, latch %u ms, no-triple hold %d ms, client follows the host's restart after %d ms (0 = never); continue1: cpsync %d, "
                 "level start hold %d (%u ms), shield %d (%u ms), follow via the stock Continue %d (wait %u ms), contreq %d (wait %u ms)", B.cfg.section_ms, B.cfg.pcs_ms,
                 B.cfg.latch_ms, B.cfg.no_triple_hold_ms, B.cfg.follow_ms, B.cfg.cp_sync, B.cfg.lstart_hold, B.cfg.lstart_ms, B.cfg.shield, B.cfg.shield_ms, B.cfg.follow_stock,
                 B.cfg.stock_wait_ms, B.cfg.cont_req, B.cfg.cont_wait_ms);
    } else if (!strcmp(sub, "cp")) {   /* continue1 (lab): this PC's campaign checkpoint record */
        unsigned r = cp_rec_addr();
        uint32_t w[9];
        if (!r || !r1_safe_read((void *)(uintptr_t)r, w, sizeof w)) snprintf(out, (size_t)size, "barrier: cp: the checkpoint record is not readable");
        else
            snprintf(out, (size_t)size, "barrier: cp: record %08X: +8 %08X mask %04X area %08X mID %d checkpoints %d / %d; sCheckPoint +0x5c %08X; save manager busy %u slot %d; the area start's movie after a restart: %s",
                     r, w[2], w[4] & 0xFFFFu, w[5], (int32_t)w[6], (int32_t)w[7], (int32_t)w[8], rd32(rd32(CHECKPT_PTR) + 0x5C), rd32(rd32(SAVEMAN_PTR) + 0xB4),
                     (int)rd32(rd32(SAVEMAN_PTR) + 0xE0), rb_start_movie_plays(0, 1, (int32_t)w[6]) ? "PLAYS (mID <= 0)" : "skipped");
    } else if (!strcmp(sub, "gflags")) {   /* rev1 live 10, LAB: barrier gflags [get | set | clear]: the two section-end game flags (set = this PC's own stock section end) */
        if (!strcmp(w1, "set") || !strcmp(w1, "clear")) {
            int ok = section_end_flags(w1[0] == 's');
            snprintf(out, (size_t)size, "barrier gflags %s: %s (LAB: %s)", w1, ok ? "done" : "NOT done (see the log)", w1[0] == 's' ? "the section-end game flags as the section's final script sets them" : "cleared");
        } else {
            unsigned holder = rd32(GF_HOLDER_PTR), n = holder ? rd32(holder + 0x28) : 0, tab = holder ? rd32(holder + 0x34) : 0;
            unsigned ie = rd32(0x113AFA0u), ic = rd32(0x113AFA4u), ir = rd32(0x113AF8Cu);
            unsigned oe = tab && ie < n ? rd32(tab + 4 * ie) : 0, oc = tab && ic < n ? rd32(tab + 4 * ic) : 0, orr = tab && ir < n ? rd32(tab + 4 * ir) : 0;
            int be = -1, bc = -1, b1 = -1, b3 = -1;
            if (oe && gf_code_ptr(rd32(rd32(oe) + 0x24))) be = ((unsigned char(TC *)(unsigned, unsigned))(uintptr_t)rd32(rd32(oe) + 0x24))(oe, 2) & 0xFF;
            if (oc && gf_code_ptr(rd32(rd32(oc) + 0x24))) bc = ((unsigned char(TC *)(unsigned, unsigned))(uintptr_t)rd32(rd32(oc) + 0x24))(oc, 2) & 0xFF;
            if (orr && gf_code_ptr(rd32(rd32(orr) + 0x24))) {
                b1 = ((unsigned char(TC *)(unsigned, unsigned))(uintptr_t)rd32(rd32(orr) + 0x24))(orr, 1) & 0xFF;
                b3 = ((unsigned char(TC *)(unsigned, unsigned))(uintptr_t)rd32(rd32(orr) + 0x24))(orr, 3) & 0xFF;
            }
            snprintf(out, (size_t)size, "barrier gflags: holder %08X (%u objects); section end: episode object %08X bit 2 = %d, cut object %08X bit 2 = %d; restart bits (object %08X): 1 = %d, 3 = %d; set %u failed %u cleared %u",
                     holder, n, oe, be, oc, bc, orr, b1, b3, g_gf_sets, g_gf_fails, g_gf_clears);
        }
    } else if (!strcmp(sub, "restart")) {
        rb_triple t = {(uint32_t)strtoul(w2, NULL, 16), atoi(w3), atoi(w4)};
        const char *more = w5;
        if (!strcmp(w1, "inject")) {
            char why[100];
            if (!rb_triple_ok(&t, why, sizeof why)) snprintf(out, (size_t)size, "barrier: not a plausible triple: %s", why);
            else {
                rb_restart_inject(&B, &t);
                snprintf(out, (size_t)size, "barrier: a host triple is waiting (area %08X, %d, %d); the next Continue of this PC uses it (hook restart is %s)", t.area, t.a, t.b, rbk_state_name(HS[RBK_RESTART].st));
            }
        } else if (!strcmp(w1, "call")) {
            unsigned self = rd32(EPISODE_PTR);
            char live[96];
            int st = rbk_probe(&rbk_defs[RBK_RESTART], &MEM, HS[RBK_RESTART].block, live, sizeof live);
            char why[100];
            if (strcmp(more, "yes")) snprintf(out, (size_t)size, "barrier: restart call changes the level for real; add 'yes' at the end to do it");
            else if (!self) snprintf(out, (size_t)size, "barrier: the episode object [%08X] is not there yet (no game running?)", EPISODE_PTR);
            else if (st == 0) snprintf(out, (size_t)size, "barrier: the restart function's bytes are %s: not called", live);
            else if (!rb_triple_ok(&t, why, sizeof why)) snprintf(out, (size_t)size, "barrier: not a plausible triple: %s", why);
            else {
                unsigned r = call_restart(&t, "console restart call");
                snprintf(out, (size_t)size, "barrier: FUN_00720870(%08X, %08X, %d, %d) returned %u (hook %s)", self, t.area, t.a, t.b, r & 0xFF, rbk_state_name(HS[RBK_RESTART].st));
            }
        } else
            snprintf(out, (size_t)size, "barrier restart inject <hex area> <a> <b> | call <hex area> <a> <b> yes");
    } else {
        snprintf(out, (size_t)size,
                 "barrier status | apply|revert [<hook>|hooks] | role off|auto | load off|auto | auto | restart inject|call ... | set section|pcs|latch|notriple|follow <ms> | set ep1only 0|1   (hooks: epend pcs movie pcsend restart start lstart cont; barrier cp = the checkpoint record)");
    }
    LeaveCriticalSection(&cs);
}
