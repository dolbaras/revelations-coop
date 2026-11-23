/*
 * cut1_game.inc - included by world1_game.c before its hook table: sync022 cutscenes (cut1.h). Two detours (both run on the thread of the movie player's update:
 * the stop is called from inside the update):
 *   WH_MPLAY  FUN_00785B30, the movie player's update (once per frame from 0x7867E3; the caller ignores its return):
 *     state 3 of a movie with its movie object (+0x2058 set) and that object ready ([[+0x2048]+0x38] == 3, what FUN_008741B0 reads): this PC's READY goes out
 *     and the update is not run (the movie stays prepared, not started) until the partner's READY of the same movie arrived, or CT_START_MS, or no session;
 *     state 5 (playing): the update runs; when this player voted first and the partner's vote has arrived, after the update this PC does the stock skip's own
 *     steps (0x78610A..0x786137 / 0x7862B7..0x7862E6): +0x20C4 = 1 unless the movie ended by itself, the stock stop FUN_00784210, +0x20AC = (state == 0).
 *   WH_MSTOP  FUN_00784210, the stock stop: when one of the update's four skip tests calls it (return address 0x786126 / 0x7861B4 / 0x7862D3 / 0x786379)
 *     right after the test set +0x20C4 = 1 (a skip press; the movie's own end leaves +0x20C4 alone), the press is this player's vote: until both voted the
 *     stop is not run, +0x20C4 goes back to 0 and the update's no-skip call FUN_00783F70 is made instead (0x786342: what every playing frame does). Every other
 *     call of the stop is passed on unchanged.
 *   The mod reads and writes no input: the game's own skip test decides what a press is (pad Start, keyboard, mouse; not while a dialog of the system
 *   [0x1169120] holds it), FACTS in cut1.h.
 *   The strips: "Waiting for %s to finish loading..." (held), "%s wants to skip the cutscene..." (the partner voted), "Waiting for %s to skip as well..." (this
 *   player voted).
 * While the session is started with the partner present and the hooks are on; `world set cut 0|1` (default 1), `world set cutskip 0|1` (default 1), `world cut`.
 * Messages: the session's own reliable event CT_EV_KIND 0x146 (READY / SKIP), sent from world_frame BEFORE the model lock and received by a link callback: live
 * s22l1 showed that the world's ext lane (CP_W1_CUT) carries nothing while a level loads (the world is not up yet), when the section's opening movie plays: both
 * PCs held 30 s. The ext lane's handler stays for an older partner. The movie state has its own lock ct_cs (the hook thread, the link callback, the frame).
 */
#include "cut1.h"

#define CT_P_STATE 0x2040u       /* the movie player's state */
#define CT_P_MOVIE 0x2044u       /* its movie object (+4 = the stream: [..]+0x38 its status: 3 ready, 0 / 7 / 8 over) */
#define CT_P_HASMOVIE 0x2058u    /* byte: the movie object is in use (else the +0x20B0 kind) */
#define CT_P_NAME 0x2064u        /* the movie's name INLINE (64 bytes; arg0 of FUN_00785580 at 0x785BFB: lea edx, [ebp+0x2064]), e.g. "92_020_0"; +0x20A4 is the stream (s22p2) */
#define CT_P_DONE 0x20ACu        /* byte: the update's last word (state == 0) */
#define CT_P_OTHER 0x20B0u       /* the other kind's object (FUN_004108C0 alive: (+0xC & 7) is 1 / 2; FUN_00891CA0 over: +0x34 && !+0x30) */
#define CT_P_SKIPPED 0x20C4u     /* byte: 1 = the movie was skipped (set by the skip test before the stop) */
#define CT_FN_SYSDLG 0x8494D0u   /* thiscall([0x1169120]): AL = a dialog of that system holds the skip (the stock test does not skip then) */
#define CT_FN_PAUSED 0x783F70u   /* the no-skip call of a playing frame (it ignores ecx): the game stays paused under the movie */
#define CT_SYS_PTR 0x1169120u
#define CT_EV_KIND 0x146u        /* CP_EV_USER + 0x46 (0x140 world, 0x141 role barrier, 0x142 downed, 0x143 pre-load digest, 0x144 give, 0x145 scan are taken) */

void menu_strip_cut(int kind, unsigned ms);   /* menu.c: the in-game strip with the partner's name (L1G_CUT_*) */

static volatile int f_cut = 1, f_cutskip = 1;
static ct_t CTS;
static CRITICAL_SECTION ct_cs;            /* CTS, the outbox, the receive log */
static volatile int ct_cs_ready;
static lk_t *ct_cb_link;                  /* the link our event callback is registered with */
static struct { ct_msg m; } ct_rxlog[8];  /* the partner's words received by the callback, logged by the frame */
static unsigned ct_nrxlog, ct_ev_tx, ct_ev_txfail, ct_ev_rx;
static int ct_last_state = -1;
static uint32_t ct_last_key;
static struct { unsigned calls, held, withheld, forced, force_blocked, second, rx, bad, lines, stops_passed, sig_bad; } CTX;
static struct { int on; uint32_t p, tid; unsigned pre_skipped; } ct_upd;   /* inside our update call (the stop hook's context) */
static int ct_sig = -1;                                                    /* the skip sites' bytes verified (1) / not (0) */
typedef unsigned (TC *ct_q0_fn)(void *self);
typedef void (TC *ct_v_fn)(void *self);
typedef void (TC *mplay_fn)(void *player);
static mplay_fn orig_mplay, orig_mstop;

/* the four skip tests' calls of the stop: E8 -> 0x784210 right before the return address, `mov [ebp+0x20C4], bl` (88 9D C4 20 00 00) before that, the
 * epilogue `cmp [ebp+0x2040], edi` (39 BD 40 20 00 00) at the return address; the no-skip call at 0x786344 (E8 -> 0x783F70) */
static const uint32_t ct_skip_rets[4] = {0x786126u, 0x7861B4u, 0x7862D3u, 0x786379u};
static int ct_check_sites(void)
{
    if (ct_sig >= 0) return ct_sig;
    int ok = 1;
    for (int i = 0; i < 4 && ok; i++) {
        uint8_t b[20];
        uint32_t r = ct_skip_rets[i];
        if (!MEM.read(MEM.user, r - 13, b, 19)) { ok = 0; break; }
        int32_t rel;
        memcpy(&rel, b + 9, 4);
        static const uint8_t mv[6] = {0x88, 0x9D, 0xC4, 0x20, 0x00, 0x00}, cmp[6] = {0x39, 0xBD, 0x40, 0x20, 0x00, 0x00};
        /* r-13: mov [ebp+0x20C4], bl (6) | r-7: mov ecx, ebp (8B CD) | r-5: call rel32 */
        ok = !memcmp(b, mv, 6) && b[6] == 0x8B && b[7] == 0xCD && b[8] == 0xE8 && (uint32_t)(r + (uint32_t)rel) == 0x784210u && !memcmp(b + 13, cmp, 6);
    }
    if (ok) {
        uint8_t c[5];
        int32_t rel;
        ok = MEM.read(MEM.user, 0x786344u, c, 5) && c[0] == 0xE8 && (memcpy(&rel, c + 1, 4), (uint32_t)(0x786349u + (uint32_t)rel) == CT_FN_PAUSED);
    }
    ct_sig = ok;
    wlog("cut: the movie player's skip tests (4 calls of the stop, the no-skip call) %s", ok ? "verified" : "NOT as expected: the skip vote is off (every stop passed on)");
    return ct_sig;
}
static int ct_is_skip_ret(uint32_t r)
{
    for (int i = 0; i < 4; i++)
        if (ct_skip_rets[i] == r) return 1;
    return 0;
}

static uint32_t ct_movie_key(uint32_t p, char *nm, size_t cap)
{
    nm[0] = 0;
    if (!MEM.read(MEM.user, p + CT_P_NAME, nm, (uint32_t)cap - 1)) return 0;
    nm[cap - 1] = 0;
    for (size_t i = 0; nm[i]; i++)
        if ((unsigned char)nm[i] < 0x20 || (unsigned char)nm[i] > 0x7E) { nm[i] = 0; break; }
    return nm[0] ? ct_key(nm) : 0;
}
/* FUN_008741B0 (ready) / FUN_008741F0 (over) of the movie object, read: [[p+0x2048]+0x38] */
static int ct_movie_ready(uint32_t p) { uint32_t s = rd32(p + CT_P_MOVIE + 4); return s && rd32(s + 0x38) == 3; }
static int ct_movie_over(uint32_t p)
{
    uint32_t s = rd32(p + CT_P_MOVIE + 4), st = s ? rd32(s + 0x38) : 0;
    return !s || st == 0 || st == 7 || st == 8;
}
static volatile int ct_partner;   /* the logic thread's last look: a started session with the partner present (the hook thread only reads it) */
static int ct_session(void) { return g_active && ct_partner; }
static ct_msg ct_out[8];
static unsigned ct_nout;
static void ct_send(const ct_msg *m)   /* any thread: into the outbox (ct_cs held by the caller) */
{
    if (ct_nout < 8) ct_out[ct_nout++] = *m;
}

/* this player voted first, the partner's vote is here: the stock skip's own steps after this frame's update (0x78610A / 0x7862B7). 0 = not now (a system
 * dialog holds the skip, the other kind's object is gone) */
static int ct_can_force(uint32_t p)
{
    uint32_t sys = rd32(CT_SYS_PTR);
    if (sys && (((ct_q0_fn)(uintptr_t)CT_FN_SYSDLG)((void *)(uintptr_t)sys) & 0xFF)) return 0;
    if (rd8(p + CT_P_HASMOVIE)) return 1;
    uint32_t o = rd32(p + CT_P_OTHER), k = o ? (rd32(o + 0xC) & 7) : 0;
    return o && (k == 1 || k == 2);
}
static void ct_force_skip(uint32_t p)
{
    int over;
    if (rd8(p + CT_P_HASMOVIE)) over = ct_movie_over(p);
    else {
        uint32_t o = rd32(p + CT_P_OTHER);
        over = o && rd8(o + 0x34) && !rd32(o + 0x30);
    }
    if (!over) wr8(p + CT_P_SKIPPED, 1);
    orig_mstop((void *)(uintptr_t)p);
    wr8(p + CT_P_DONE, rd32(p + CT_P_STATE) == 0);
}

static void TC hook_mplay(void *self)
{
    uint32_t p = (uint32_t)(uintptr_t)self;
    CTX.calls++;
    if (!ct_cs_ready || !g_active || !f_cut || in_ops()) {
        orig_mplay(self);
        return;
    }
    int st = (int)rd32(p + CT_P_STATE);
    char nm[24];
    nm[0] = 0;
    uint32_t key = (st == 3 || st == 4 || st == 5) ? ct_movie_key(p, nm, sizeof nm) : 0;
    uint32_t now = GetTickCount();
    int sess = ct_session();
    int ready = st == 3 && rd8(p + CT_P_HASMOVIE) && ct_movie_ready(p);
    EnterCriticalSection(&ct_cs);
    if (st == 3 && (ct_last_state != 3 || key != ct_last_key) && key) {   /* a movie is being prepared: a new start */
        ct_begin(&CTS, key, now);
        if (CTX.lines++ < 120) wlog("cut: movie '%s' (key %08x) prepared here (start #%u of it)", nm, (unsigned)key, CTS.n);
        split_gate_request("a movie is prepared");
    }
    ct_last_state = st, ct_last_key = key;
    if (st == 0 || st > 5) ct_end(&CTS);
    if (ready && CTS.have && !CTS.released) {
        ct_msg m;
        int snd = 0;
        int r = ct_ready_step(&CTS, sess, now, &m, &snd);
        if (snd) {
            ct_send(&m);
            if (CTX.lines++ < 120) wlog("cut: movie '%s' ready here: READY sent, the playback waits for the partner (at most %u ms)", nm, CT_START_MS);
        }
        if (r == CT_HOLD) {
            CTX.held++;
            LeaveCriticalSection(&ct_cs);
            menu_strip_cut(1, 600);
            return;   /* the update is not run: the movie stays prepared */
        }
        if (CTX.lines++ < 120)
            wlog("cut: movie '%s' starts here: %s after %u ms", nm, ct_peer_ready_has(&CTS, CTS.key) ? "the partner's READY arrived" : sess ? "the time limit" : "no partner session",
                 (unsigned)(now - (CTS.t_ready ? CTS.t_ready : now)));
    }
    LeaveCriticalSection(&ct_cs);
    int vote = st == 5 && f_cutskip && ct_check_sites() == 1;
    if (vote) ct_upd.p = p, ct_upd.tid = GetCurrentThreadId(), ct_upd.pre_skipped = rd8(p + CT_P_SKIPPED), ct_upd.on = 1;
    orig_mplay(self);
    ct_upd.on = 0;
    if (!vote || rd32(p + CT_P_STATE) != 5) return;
    /* still playing: the partner's vote completes this player's earlier one -> skip together; else the strips */
    int force = 0, pw = 0, sw = 0;
    uint32_t k = 0;
    EnterCriticalSection(&ct_cs);
    if (CTS.have && !CTS.skip_done && CTS.vote_sent && ct_peer_vote_has(&CTS, CTS.key)) {
        if (ct_can_force(p)) {
            ct_msg m;
            int snd = 0;
            force = ct_skip_step(&CTS, sess, 0, &m, &snd) == 2;
        } else CTX.force_blocked++;
    }
    pw = ct_partner_waits(&CTS), sw = ct_self_waits(&CTS), k = CTS.key;
    LeaveCriticalSection(&ct_cs);
    if (force) {
        ct_force_skip(p);
        CTX.forced++;
        if (CTX.lines++ < 120)
            wlog("cut: the partner's vote completed this player's: the movie (key %08x) is skipped together through the stock steps (skipped flag %u, state now %u)", (unsigned)k,
                 rd8(p + CT_P_SKIPPED), rd32(p + CT_P_STATE));
        return;
    }
    if (pw) menu_strip_cut(2, 600);
    else if (sw) menu_strip_cut(3, 600);
}

/* the stock stop: from a skip test inside our update call = this player's skip press */
static void TC hook_mstop(void *self)
{
    uint32_t p = (uint32_t)(uintptr_t)self, ret = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (!ct_cs_ready || !ct_upd.on || ct_upd.p != p || ct_upd.tid != GetCurrentThreadId() || !ct_is_skip_ret(ret) || rd8(p + CT_P_SKIPPED) != 1 || ct_upd.pre_skipped != 0 || !g_active || !f_cut ||
        !f_cutskip) {
        CTX.stops_passed++;
        orig_mstop(self);
        return;
    }
    int sess = ct_session(), r = 0;
    ct_msg m;
    int snd = 0;
    uint32_t k;
    EnterCriticalSection(&ct_cs);
    if (CTS.have) r = ct_skip_step(&CTS, sess, 1, &m, &snd);
    if (snd) ct_send(&m);
    k = CTS.key;
    LeaveCriticalSection(&ct_cs);
    if (snd && CTX.lines++ < 120) wlog("cut: this player wants to skip the movie (key %08x): vote sent to the partner", (unsigned)k);
    if (r == 1) {   /* the vote is out, the partner's is not: no stop, the playing frame's own call instead */
        wr8(p + CT_P_SKIPPED, 0);
        ((ct_v_fn)(uintptr_t)CT_FN_PAUSED)(self);
        CTX.withheld++;
        menu_strip_cut(3, 3000);
        return;
    }
    if (r == 2) {
        CTX.second++;
        if (CTX.lines++ < 120) wlog("cut: both players voted: the movie (key %08x) is skipped through the stock stop (this press was the second vote)", (unsigned)k);
    }
    orig_mstop(self);
}

/* the partner's word on the session's event lane (the session's thread, the link lock held): into CTS at once (the hook may be holding a movie for it), logged
 * later by the frame */
static void cut_on_event(void *user, const cp_event *e)
{
    (void)user;
    ct_msg m;
    if (!ct_cs_ready || !e || e->kind != CT_EV_KIND) return;
    EnterCriticalSection(&ct_cs);
    ct_ev_rx++;
    if (ct_dec(e->body, e->len, &m)) {
        ct_rx(&CTS, &m, GetTickCount());
        if (ct_nrxlog < 8) ct_rxlog[ct_nrxlog++].m = m;
    } else CTX.bad++;
    LeaveCriticalSection(&ct_cs);
}

/* world_frame, BEFORE the model lock (the link calls take the net / link locks): the partner's presence for the hooks, the callback, the outbox, the log */
static void cut_pass(int active)
{
    if (!ct_cs_ready) InitializeCriticalSection(&ct_cs), ct_cs_ready = 1;
    lk_t *k = net1_link_peek();
    int part = 0;
    if (k && active) {
        lk_info in;
        memset(&in, 0, sizeof in);
        lk_get_info(k, &in);
        part = in.started && in.remote_role >= 0;
        if (ct_cb_link != k && lk_add_event_cb(k, cut_on_event, NULL)) ct_cb_link = k;
    }
    ct_partner = part;
    ct_msg o[8], lg[8];
    unsigned n, nl;
    EnterCriticalSection(&ct_cs);
    n = ct_nout, nl = ct_nrxlog;
    memcpy(o, ct_out, sizeof o);
    for (unsigned i = 0; i < nl && i < 8; i++) lg[i] = ct_rxlog[i].m;
    ct_nout = 0, ct_nrxlog = 0;
    LeaveCriticalSection(&ct_cs);
    cs_t *cs = k && part ? lk_session(k) : NULL;
    for (unsigned i = 0; i < n; i++) {
        uint8_t b[CT_LEN];
        ct_enc(b, &o[i]);
        if (cs && cs_send_event(cs, CT_EV_KIND, b, CT_LEN)) ct_ev_tx++;
        else ct_ev_txfail++;
    }
    for (unsigned i = 0; i < nl; i++)
        if (CTX.lines++ < 120) wlog("cut: the partner's %s for movie key %08x (his start #%u)", lg[i].kind == CT_READY ? "READY" : "SKIP vote", (unsigned)lg[i].key, lg[i].n);
}

/* a word of the partner (op_ext, logic thread) */
static void cut_rx(const uint8_t *b, unsigned len)
{
    ct_msg m;
    CTX.rx++;
    if (!ct_cs_ready) return;
    if (!ct_dec(b, len, &m)) {
        CTX.bad++;
        return;
    }
    EnterCriticalSection(&ct_cs);
    ct_rx(&CTS, &m, GetTickCount());
    LeaveCriticalSection(&ct_cs);
    if (CTX.lines++ < 120) wlog("cut: the partner's %s for movie key %08x (his start #%u)", m.kind == CT_READY ? "READY" : "SKIP vote", (unsigned)m.key, m.n);
}

/* a new level instance: the next state 3 is a new start; the partner's words stay (matched by key and arrival time: his READY may come before this PC's epoch) */
static void cut_reset(void)
{
    if (!ct_cs_ready) return;
    EnterCriticalSection(&ct_cs);
    ct_last_state = -1, ct_last_key = 0;
    LeaveCriticalSection(&ct_cs);
}

static void cut_status(char *out, int size)
{
    snprintf(out, (size_t)size,
             "world cut (cut1.h): cut %d, cutskip %d, skip sites %s | player updates %u | movie starts %u, holds %u (%u frames), released by the partner %u / the time limit %u / no session %u | "
             "votes here %u, from the partner %u, presses withheld %u, skipped together %u (second press %u, completed by the partner's vote %u, blocked %u) | other stops %u | words %u (bad %u), "
             "session events sent %u (not sent %u) received %u | "
             "now: %s key %08x start #%u released %d voted %d",
             f_cut, f_cutskip, ct_sig < 0 ? "not checked" : ct_sig ? "verified" : "NOT verified", CTX.calls, CTS.starts, CTS.holds, CTS.held_frames, CTS.released_peer, CTS.released_timeout,
             CTS.released_nosession, CTS.votes_local, CTS.votes_peer, CTX.withheld, CTS.skips, CTX.second, CTX.forced, CTX.force_blocked, CTX.stops_passed, CTX.rx, CTX.bad, ct_ev_tx, ct_ev_txfail, ct_ev_rx,
             CTS.have ? "a movie" : "no movie", (unsigned)CTS.key, CTS.n, CTS.released, CTS.vote_sent);
}
