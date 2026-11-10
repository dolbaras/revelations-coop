/*
 * puppet_game.c - the game side of the network puppet (B5): reads this PC's pad and character, sends CP_PAD and the Revelations 1 STATE, applies the partner's pad records through
 * capture_push_remote and corrects the puppet's position. The logic itself (order, loss, holds, correction thresholds) is puppet1.c and is tested offline; this file only holds the memory
 * access of build 13298487 (notes\net-puppet-re.md) and the console command `net pad`.
 *
 * ON by default since lobby-must (REV1_PUPPET=0 / `net pad off` switch it off); it acts only in a started network session, so split screen and everything else behave as before. All reads and writes happen on the game's
 * logic thread (lt.c frame callback), as the research demands (8 unit threads).
 *
 *   net pad on [solo]   start; solo = no session needed: the partner's records come from `net pad inject` (one-instance live checks)
 *   net pad off         stop and give the roles back
 *   net pad status      counters of the sender and the receiver
 *   net pad inject <hex buttons> <lt> <rt> <lx> <ly> <rx> <ry>   a packet from the partner (records the receiver as if the network had delivered it)
 *   net pad state <x> <y> <z> <yaw> [hex flags]   a STATE sample of the partner (position target of the correction)
 *   net pad hold <hex flags>   the partner's flags (2 pause, 8 ... see CP_SF_*), 0 = release
 *   net pad stall <ms>  drop incoming packets for ms (a link cut)
 *   net pad auto <ms> <hex buttons> <lt> <rt> <lx> <ly> <rx> <ry>   repeat that injection in EVERY frame pass for ms (60 Hz; the ssh console cannot); `auto 0` stops it
 *   net pad watch <ms>   log the puppet unit's position per pass (moves only + 1 in 30): tells an engine / script put-back from our own correction (`net pad status`: foreign moves)
 *   net pad set unlock 1   TEST ONLY (default 0): clear the "input prohibited" bit 20 of the puppet unit's +0xF3C every pass (updatePad zeroes all sticks of a unit with that bit)
 *   net pad set kbskip -1|0|1   keep the partner's pad (pad slot 1) through the engine's keyboard-mode wipe (FUN_0088CB10: with sGamePad+0x874 set it zeroes the raw data of BOTH pads every frame and
 *                               refills the current slot from the keyboard); the keyboard keeps working for this PC's own slot (menus, pause). -1 automatic = whenever the roles are bound,
 *                               solo and network (default), 0 never, 1 always (the first version skipped the whole function and was solo-only: it took the keyboard away)
 *   net pad kbreset   leave keyboard mode now (+0x874/+0x878 = 0, PadData kind 6 -> the previous kind); keyboard / mouse activity sets it again
 *   net pad rearm   pad slot 1 is given back to the game for 2.5 s and then remote again: the game releases and re-assigns Pad[1] (the old "pad 1 off / pad 1 0" re-arm)
 *   net pad set keep0 -1|0|1   keep the game's XInput user 0 connected (neutral synthetic pad) while the puppet runs: -1 automatic = solo, or no gamepad on user 0 at the bind (a keyboard player;
 *                               lobby-must default), 0 never, 1 always; applies at the next bind
 *   net pad set keepalive|eps|redundancy|soft|hard|jump|neutral <n>   |   net pad set ignorepause 1   (TEST ONLY, default 0: this PC's own pause / menu / message-box flag no longer holds the puppet)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xinput.h>

#include "capture.h"
#include "downed1.h"
#include "net1.h"
#include "puppet1.h"
#include "r1.h"
#include "solo1.h"
#include "grab1.h"   /* ep1-solo E11: the own human held by an enemy */
#include "world1_game.h"   /* ep1-solo: solo1_cam_gameplay, solo1_unit_off */

#define TC __attribute__((thiscall))

#define CAMROOT_PTR 0x11E00B8u
#define COCKPIT_PTR 0x11E0184u
#define FN_REFRESH 0x6BEF50u   /* FUN_006BEF50(unit, mode, 1): unit refresh after a move (probe.c `tp`) */

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((void *)(size_t)a, &v, 4);
    return v;
}
static unsigned rd8(unsigned a)
{
    unsigned char v = 0;
    r1_safe_read((void *)(size_t)a, &v, 1);
    return v;
}
static float rdf(unsigned a)
{
    float v = 0;
    r1_safe_read((void *)(size_t)a, &v, 4);
    return v;
}
static int wr(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)(size_t)a, p, n, &got) && got == n;
}
static int wr8(unsigned a, unsigned v)
{
    unsigned char c = (unsigned char)v;
    return wr(a, &c, 1);
}
static int wrf(unsigned a, float v) { return wr(a, &v, 4); }
static int wr32(unsigned a, unsigned v) { return wr(a, &v, 4); }

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

/* ---- state of the module ---- */
static volatile int g_on, g_solo;
static int g_roles_bound, g_bound_role = -1;
static p1_tx g_tx;
static p1_rx g_rx;
static p1_tx_cfg g_txc;
static p1_rx_cfg g_rxc;
static p1_corr_cfg g_cc;
static int g_inited;
static volatile int g_kbskip = -1;                 /* keep the partner's pad through the engine's keyboard-mode wipe while the roles are bound: -1 = automatic (= on, solo and network), 0 never, 1 always */
static volatile int g_unlock;                      /* TEST ONLY (net pad set unlock 1): clear the "input prohibited" bit of the puppet unit (+0xF3C bit 20) every pass */
static uint32_t g_unlock_n;
/* watch / foreign move detection (live: Parker walks a few units and is put back to the start point exactly, while our counters say tp 0: the engine or the opening scene does it) */
static int g_tp_this_pass;                         /* our own teleport ran in this pass */
static int g_pos_have;
static float g_pos_prev[3];
static uint32_t g_foreign_n, g_foreign_frame, g_foreign_ms;
static float g_foreign_from[3], g_foreign_to[3], g_foreign_d;
static uint32_t g_watch_until, g_watch_pass, g_watch_start;
static unsigned g_watch_unit_seen;
static volatile int g_keep0 = -1;                  /* keep the game's XInput user 0 connected (a neutral synthetic pad) while the puppet runs: -1 = automatic (solo only), 0 = never, 1 = always */
static int g_keep0_on;                             /* the neutral pad of user 0 is ours right now */
static uint32_t g_rearm_until;                     /* `net pad rearm`: pad slot 1 is given back to the game (native) until this tick, then remote again: the game assigns a new Pad[1] */
static int g_rearm_on;
static volatile unsigned g_scene_flags;             /* flags of this PC in the last pass (puppet_scene_now) */
static volatile DWORD g_scene_tick;
static int g_pup_off_seen;                         /* ep1-solo: the puppet was switched off by the story in the last pass */
static volatile int g_ignorepause;                 /* TEST ONLY: this PC's own gui flag does not hold the puppet */
static uint32_t g_auto_until;                      /* `net pad auto`: inject every pass until this tick */
static cp_pad_rec g_auto_rec;
static volatile int g_auto_on;
static uint32_t g_auto_count;
static void inject_flags_and_rec(const cp_pad_rec *rec);
static unsigned g_epoch_seen = 0xFFFFFFFFu;
static unsigned g_sink_pad = 1;
static uint16_t g_inject_seq;
static uint16_t g_inject_flags, g_inject_state_flags;
static uint32_t g_stall_until;
static float g_prev_pos[3];
static int g_have_prev;
static uint32_t g_frames, g_state_sent;
static p1_corr_result g_last_corr;
static uint32_t g_last_log;
static char g_last_tp[120];

static void ensure_init(void)
{
    if (g_inited) return;
    p1_tx_defaults(&g_txc);
    p1_rx_defaults(&g_rxc);
    p1_corr_defaults(&g_cc);
    g_inited = 1;
}

static int sink_push(void *u, const cp_pad_rec *rec, uint32_t seq, uint32_t ts)
{
    (void)u;
    return capture_push_remote((int)g_sink_pad, (const r1_input_t *)rec, seq, ts);
}

static int unit_get(void *u, float pos[3], float *yaw)
{
    unsigned unit = unit_by_slot((unsigned)(size_t)u);
    if (!unit) return 0;
    pos[0] = rdf(unit + 0x40), pos[1] = rdf(unit + 0x44), pos[2] = rdf(unit + 0x48), *yaw = rdf(unit + 0x1180);
    return 1;
}

/* the console `tp` of probe.c (LIVE stage 6c): the move object's teleport byte, the position, +0x4C = 0, the refresh call, all in this one logic-thread pass; plus the heading */
static int unit_tp(void *u, const float pos[3], float yaw)
{
    unsigned unit = unit_by_slot((unsigned)(size_t)u), mo = unit ? rd32(unit + 0xE88) : 0;
    if (!unit || !mo) return 0;
    wr8(mo + 0x2B4, 1);
    wrf(unit + 0x40, pos[0]), wrf(unit + 0x44, pos[1]), wrf(unit + 0x48, pos[2]);
    wr32(unit + 0x4C, 0);
    ((unsigned(TC *)(unsigned, unsigned, unsigned))FN_REFRESH)(unit, rd32(unit + 0xF08), 1);
    wrf(unit + 0x1180, yaw);
    g_tp_this_pass = 1;
    snprintf(g_last_tp, sizeof g_last_tp, "unit %08x to (%.1f, %.1f, %.1f) yaw %.2f at %lu", unit, pos[0], pos[1], pos[2], yaw, (unsigned long)GetTickCount());
    r1_log("puppet: teleport %s", g_last_tp);
    return 1;
}

/* a full-screen GUI that the engine draws in every viewport (same test as split.c): 1 = pause / game over, 2 = item / map menu */
static int g_gui_src;   /* what made gui_running() answer last: 0 none, 2 / 3 = cockpit manager k (game over / pause), 100 = the item / map menu manager */
static unsigned g_gui_flags;
static unsigned g_gui_stuck;   /* bit i: source i has its running bits set and has never been seen clear (live N100: the game over manager of the new-game opening reads 000f4c02 all the time): ignored */

/* The stock test (same as split.c): a full-screen GUI manager RUNS when its unit flags have all the bits of a mask (0xC00 for the game over / pause managers 2 and 3, 0x4800 for the item menu
 * manager); at rest they are clear (pause 0x60B2 -> 0x6CB2 when it opens). Live (N100, new game, ~60 s after the start) the manager 2 reads 000f4c02 during completely free play, so the bare
 * bit test is a false positive there. A source therefore counts as running only after it was SEEN with the bits clear (armed) and then set: a real pause / game over / item menu is always a
 * rising edge after a clear state. A pointer change (new level, units recreated) disarms it again. 1 = pause / game over, 2 = item / map menu, 0 = none. */
static int gui_running(void)
{
    static unsigned last_ptr[3];
    static int armed[3];
    unsigned ck = rd32(COCKPIT_PTR), objs[3] = {0, 0, 0}, masks[3] = {0xC00, 0xC00, 0x4800};
    if (ck) {
        objs[0] = rd32(ck + 0x28 + 4 * 2), objs[1] = rd32(ck + 0x28 + 4 * 3), objs[2] = rd32(ck + 0x3C);
    }
    int res = 0;
    g_gui_src = 0, g_gui_flags = 0, g_gui_stuck = 0;
    for (int i = 0; i < 3; i++) {
        if (objs[i] != last_ptr[i]) last_ptr[i] = objs[i], armed[i] = 0;
        if (!objs[i]) continue;
        unsigned fl = rd32(objs[i] + 0xC);
        if ((fl & masks[i]) != masks[i]) {
            armed[i] = 1;
            continue;
        }
        if (!armed[i]) {
            g_gui_stuck |= 1u << i;
            continue;
        }
        if (!res) {
            res = i == 2 ? 2 : 1;
            g_gui_src = i == 2 ? 100 : i + 2, g_gui_flags = fl;
        }
    }
    return res;
}

/* diagnostics for `net pad status`: the unit flags of every cockpit manager 0..7, the item menu manager, and the message boxes of the singleton [0x1182CA4] (+0xFC main, +0x100 sub; the
 * prompt / hint window of the first minutes is one of these); the stock pause test above only looks at managers 2, 3 and the item menu */
static void gui_dump(char *out, size_t size)
{
    unsigned ck = rd32(COCKPIT_PTR);
    size_t n = 0;
    out[0] = 0;
    for (unsigned k = 0; ck && k < 8 && n < size; k++) {
        unsigned g = rd32(ck + 0x28 + 4 * k);
        n += (size_t)snprintf(out + n, size - n, "%s%u:%08x", k ? " " : "", k, g ? rd32(g + 0xC) : 0);
    }
    unsigned sub = ck ? rd32(ck + 0x3C) : 0, mb = rd32(0x1182CA4u);
    unsigned mm = mb ? rd32(mb + 0xFC) : 0, ms = mb ? rd32(mb + 0x100) : 0;
    if (n < size) snprintf(out + n, size - n, " item:%08x msgbox main:%08x sub:%08x", sub ? rd32(sub + 0xC) : 0, mm ? rd32(mm + 0xC) : 0, ms ? rd32(ms + 0xC) : 0);
}

/* scene flags of THIS PC: loading (no gameplay cameras), cutscene (viewport 0 shows another camera), pause / menus */
static uint16_t scene_flags(unsigned own_unit)
{
    uint16_t f = 0;
    unsigned root = rd32(CAMROOT_PTR), mgr0 = root ? rd32(root + 0xCE0) : 0;
    if (!root || !mgr0 || !own_unit) f |= CP_SF_LOADING;
    else if (!solo1_cam_gameplay(rd32(root + 0x30 + 4), mgr0)) f |= CP_SF_CUTSCENE;   /* ep1-solo: manager 1 in viewport 0 while this PC watches is gameplay */
    if (gui_running() && !g_ignorepause) f |= CP_SF_PAUSED;
    return f;
}

static int snapshot(unsigned unit, p1_snapshot *s)
{
    memset(s, 0, sizeof *s);
    if (!unit) return 0;
    s->valid = 1;
    s->pos[0] = rdf(unit + 0x40), s->pos[1] = rdf(unit + 0x44), s->pos[2] = rdf(unit + 0x48), s->yaw = rdf(unit + 0x1180);
    s->hp = rdf(unit + 0xE38), s->hpmax = rdf(unit + 0xE40);
    unsigned mach = unit + 0xF6C, inner = rd32(mach + 4);
    s->upper_id = inner ? rd32(inner + 0x18) : 0;
    s->mode = rd32(unit + 0xF08), s->pstate = rd32(unit + 0xF3C), s->ctrl = rd32(unit + 0xF44), s->unit_flags = rd32(unit + 0xE1C);
    unsigned pack = rd32(unit + 0xFE8);
    s->weapon = pack ? (int)rd8(pack + 0xD4) : 0xF;
    s->aiming = (int)rd8(unit + 0x1151);
    return 1;
}

/* kbskip keeps only the partner's pad (capture.c / kbskip1.h): this PC's keyboard still drives its own slot, so it is safe in network play too (a keyboard / mouse player would otherwise wipe
 * the partner's pad in every frame of keyboard mode: any mouse move sets the mode, only a held bit on pad 0 clears it) */
static void kbskip_bind(void)
{
    capture_set_kbskip_pads(1u << g_sink_pad);
    capture_set_kbskip(g_kbskip != 0);
}

/* Why user 0 must stay connected (static, sPad_PollPad 0x9C0670 / sPad_ScanPad 0x9C04B0): the game scans for a free XInput user for Pad[k] once a second, but a Pad with kind 0 is scanned
 * only when every LOWER Pad already has a kind (PollPad: `if kind(Pad[j]) == 0 return` for j < k), and a Pad whose user stops answering XInputGetState is released at once (FUN_009befe0,
 * kind -> 0). capture_set_source(0, CAP_NATIVE) switches the synthetic pad of user 0 OFF: on a PC without a real pad (the N100) Pad[0] then lives only while a console `padhold 0` runs, and
 * Pad[1] never gets its turn (live: Pad[0] kind 4, Pad[1] kind 0 although pad slot 1 reported "remote, accepted"). stage 3 / 6b had `pad 0 0` + `pad 1 0` (both synthetic or both real). */
static void bind_roles(int role)
{
    capture_bind(role, 0);
    capture_bind(1 - role, 1);
    capture_set_source(0, CAP_NATIVE);
    kbskip_bind();
    XINPUT_GAMEPAD real0;
    int pad0 = testmode_pad_peek(0, &real0);   /* before our own neutral pad: a real gamepad on user 0 (or none: keyboard / mouse player) */
    if (g_keep0 == 1 || (g_keep0 < 0 && (g_solo || !pad0))) {   /* automatic: solo, or a PC without a gamepad on user 0 (else Pad[1] is never assigned: scan order) */
        static const r1_input_t zero;
        testmode_pad_set(0, 1, &zero);   /* user 0 stays connected and neutral (a console padhold 0 still works: it falls back to this) */
        g_keep0_on = 1;
    }
    capture_set_source((int)g_sink_pad, CAP_REMOTE);
    capture_set_remote_timeout(1000);   /* longer than the neutral timeout of puppet1 (400 ms) and its keepalive refresh */
    g_roles_bound = 1, g_bound_role = role;
    r1_log("puppet: roles bound: this PC plays unit slot %d on pad 0, the partner (slot %d) is driven by pad slot %u (remote records)", role, 1 - role, g_sink_pad);
}

static void release_roles(void)
{
    if (!g_roles_bound) return;
    if (g_keep0_on) {
        static const r1_input_t zero;
        testmode_pad_set(0, 0, &zero);
        g_keep0_on = 0;
    }
    g_rearm_on = 0;
    capture_set_kbskip(0);
    capture_set_source((int)g_sink_pad, CAP_NATIVE);
    capture_set_remote_timeout(0);
    capture_unbind(-1);
    g_roles_bound = 0, g_bound_role = -1;
    r1_log("puppet: roles released");
}

static void start_epoch(unsigned epoch)
{
    g_epoch_seen = epoch;
    p1_tx_restart(&g_tx);
    g_have_prev = 0;
}

void puppet_frame(void)
{
    if (!g_on) {
        if (g_roles_bound) release_roles();
        return;
    }
    ensure_init();
    g_frames++;
    DWORD now = GetTickCount();
    lk_t *lk = net1_link_peek();
    cs_t *cs = lk && !g_solo ? lk_session(lk) : NULL;
    int role = 0;
    unsigned epoch = 1;
    int playing = 0;
    if (g_solo) {
        playing = 1;
    } else if (cs && cs_active(cs)) {
        lk_info in;
        lk_get_info(lk, &in);
        role = in.role < 0 ? 0 : in.role;
        epoch = cs_epoch(cs);
        /* lobby-must (S3): the partner has left (the host's link waits again, the session stays "started"): the roles are given back, so the partner's unit gets
         * its saved control type, kind and upper-body mode again (on the host: 2 = the AI companion it was created as) */
        playing = cs_phase(cs) == CP_PHASE_PLAYING && in.started && cs_remote_pid(cs) >= 0;
    }
    if (!playing) {
        if (g_roles_bound) release_roles();
        g_epoch_seen = 0xFFFFFFFFu;
        return;
    }
    if (!g_roles_bound || g_bound_role != role) bind_roles(role);
    if (epoch != g_epoch_seen) start_epoch(epoch);
    /* round 2: automatic keep0 follows a gamepad plugged in (or out) after the bind: the neutral stand-in on user 0 must not mask the player's own pad.
     * Once a second, two equal answers in a row before a change. */
    {
        static DWORD last_check;
        static int votes;
        if (g_keep0 < 0 && now - last_check >= 1000) {
            last_check = now;
            int want = p1_keep0_want(g_keep0, g_solo, testmode_pad_real_present(0));
            if (want != g_keep0_on && ++votes >= 2) {
                static const r1_input_t zero;
                testmode_pad_set(0, want, &zero);
                g_keep0_on = want, votes = 0;
                r1_log("puppet: %s", want ? "no gamepad on user 0 any more: the neutral stand-in keeps user 0 connected again"
                                          : "a gamepad appeared on user 0: the neutral stand-in is off, the player's own pad drives user 0");
            } else if (want == g_keep0_on)
                votes = 0;
        }
    }

    unsigned own = unit_by_slot((unsigned)role), pup = unit_by_slot((unsigned)(1 - role));
    uint16_t flags = scene_flags(own);
    if (downed1_own_down()) flags |= CP_SF_DOWN;   /* the own human lies (downed rule, network mode): the partner holds his puppet of him (no pad drive, no position correction) */
    {   /* ep1-solo E11: the own human is held by an enemy (+0xE34 of the grab states): the partner holds his puppet of him (the break-free waggle is no walk) */
        static gr1_t gr;
        int was = gr.held, cap0 = gr.caps;
        if (gr1_step(&gr, own, own ? rd32(own + GR1_OFF_HELD) : 0, now, GR1_CAP_MS)) flags |= CP_SF_HELD;
        if (gr.held != was || gr.caps != (unsigned)cap0)
            r1_log("puppet: the own human (unit %08x) is %s", own, gr.held ? "HELD by an enemy (+0xE34 set): the partner holds his puppet of him" : gr.caps != (unsigned)cap0 ? "still marked held after the cap: the held flag is dropped" : "free again");
    }
    g_scene_flags = flags, g_scene_tick = now;

    /* ---- the puppet unit's position every pass: a move that is not ours (script / engine put-back) is counted and remembered; `net pad watch` logs the pass lines ---- */
    {
        float cur[3] = {0, 0, 0};
        if (pup) cur[0] = rdf(pup + 0x40), cur[1] = rdf(pup + 0x44), cur[2] = rdf(pup + 0x48);
        if (pup && g_pos_have && !g_tp_this_pass) {
            float dx = cur[0] - g_pos_prev[0], dy = cur[1] - g_pos_prev[1], dz = cur[2] - g_pos_prev[2], d = sqrtf(dx * dx + dz * dz);
            if (d > 30.0f || fabsf(dy) > 60.0f) {   /* walking is ~2 units per pass; 30 is a jump */
                g_foreign_n++, g_foreign_frame = g_frames, g_foreign_ms = now, g_foreign_d = d;
                memcpy(g_foreign_from, g_pos_prev, sizeof g_pos_prev), memcpy(g_foreign_to, cur, sizeof cur);
                r1_log("puppet: FOREIGN move of the puppet unit %08x: (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f), %.1f units in one pass (pass %u) - not our teleport (our tp count %u)", pup, g_pos_prev[0],
                       g_pos_prev[1], g_pos_prev[2], cur[0], cur[1], cur[2], d, g_frames, g_rx.st.tp_total);
            }
        }
        if (pup && g_unlock) {
            unsigned f3c = rd32(pup + 0xF3C);
            if (f3c & 0x100000u) wr32(pup + 0xF3C, f3c & ~0x100000u), g_unlock_n++;
        }
        if (pup && (int32_t)(g_watch_until - now) > 0) {
            float dx = g_pos_have ? cur[0] - g_pos_prev[0] : 0, dz = g_pos_have ? cur[2] - g_pos_prev[2] : 0;
            g_watch_pass++;
            if (dx != 0 || dz != 0 || g_tp_this_pass || g_watch_pass % 30 == 0)
                r1_log("puppet-watch: pass %u t+%lu pos (%.2f, %.2f, %.2f) d(%.2f, %.2f)%s mode %x flags %08x F3C %08x%s F44 %08x stick(%.2f, %.2f) aim %u ctl %u", g_frames, (unsigned long)(now - g_watch_start), cur[0], cur[1], cur[2],
                       dx, dz, g_tp_this_pass ? " OUR-TP" : "", rd32(pup + 0xF08), rd32(pup + 0xE1C), rd32(pup + 0xF3C), (rd32(pup + 0xF3C) & 0x100000u) ? " INPUT-BAN" : "", rd32(pup + 0xF44), rdf(pup + 0x1138), rdf(pup + 0x113C), rd8(pup + 0x1151), rd32(rd32(rd32(pup + 0xE80) + 0x38) + 0x34));
        }
        if (pup) memcpy(g_pos_prev, cur, sizeof cur), g_pos_have = 1;
        else g_pos_have = 0;
        g_tp_this_pass = 0;
        g_watch_unit_seen = pup;
    }

    /* ---- send: the local pad (what the game would read from XInput user 0) and the character's STATE ---- */
    if (cs) {
        cp_pad_rec cur;
        memset(&cur, 0, sizeof cur);
        if (!testmode_pad_peek(0, &cur)) memset(&cur, 0, sizeof cur);
        cp_pad pk;
        if (p1_tx_step(&g_tx, now, &cur, (uint16_t)(flags & CP_PAD_FLAGS_MASK), &pk)) cs_send_pad(cs, &pk);
        p1_snapshot sn;
        if (snapshot(own, &sn) && !(flags & CP_SF_LOADING)) {
            cp_state st;
            if (p1_state_fill(&st, &sn, flags, cur.rt, g_have_prev ? g_prev_pos : NULL) && cs_submit_state(cs, &st)) g_state_sent++;
            memcpy(g_prev_pos, sn.pos, sizeof g_prev_pos), g_have_prev = 1;
        }
    }

    /* ---- `net pad rearm`: the synthetic pad of slot 1 goes away for 2.5 s (the game releases Pad[1]) and comes back (the game scans again and assigns it) ---- */
    if (g_rearm_on && (int32_t)(now - g_rearm_until) >= 0) {
        g_rearm_on = 0;
        capture_set_source((int)g_sink_pad, CAP_REMOTE);
        r1_log("puppet: rearm: pad slot %u is remote again", g_sink_pad);
    }

    /* ---- `net pad auto`: one injected record per pass, as a partner pad at 60 Hz would give ---- */
    if (g_auto_on) {
        if ((int32_t)(g_auto_until - now) > 0) inject_flags_and_rec(&g_auto_rec), g_auto_count++;
        else g_auto_on = 0;
    }

    /* ---- receive ---- */
    if (cs) {
        cp_pad pk;
        int rp = cs_remote_pid(cs);
        while (cs_recv_pad(cs, &pk)) {
            if ((int)pk.pid != rp || (int)(g_stall_until - now) > 0) continue;
            p1_rx_packet(&g_rx, now, &pk, epoch);
        }
        cp_state rs;
        int r = rp >= 0 ? cs_remote_sample(cs, rp, cs_now(cs), &rs) : CS_SAMPLE_NONE;
        if (r == CS_SAMPLE_INTERP || r == CS_SAMPLE_EXTRAP) p1_rx_state(&g_rx, now, 1, rs.pos, rs.yaw, rs.flags, rs.t_ms, rs.epoch);
        else p1_rx_state(&g_rx, now, 0, NULL, 0, 0, 0, epoch);
    } else if (g_solo) {
        /* the state target of a solo run is whatever `net pad state` set (p1_rx_state keeps it); its age is refreshed so that it does not go stale */
        if (g_rx.tgt.valid) p1_rx_state(&g_rx, now, 1, g_rx.tgt.pos, g_rx.tgt.yaw, g_inject_state_flags, g_rx.tgt.t_ms, epoch);
    }
    /* ep1-solo: a puppet the story switched off here (1-3 solo part) belongs to the story like in a cutscene: no pad replay, no correction; one teleport when he is back */
    int pup_off = pup && solo1_unit_off(pup);
    if (pup_off != g_pup_off_seen) {
        g_pup_off_seen = pup_off;
        r1_log("puppet: the partner's puppet unit %08x is %s", pup, pup_off ? "switched off by the story here: held (no pad replay, no position correction)" : "back in play: the pad replay and the correction run again");
    }
    p1_rx_tick(&g_rx, now, epoch, s1_puppet_hold((uint16_t)(flags & (CP_SF_LOADING | CP_SF_PAUSED | CP_SF_CUTSCENE)), pup_off, CP_SF_CUTSCENE));
    p1_unit_ops ops = {(void *)(size_t)(1 - role), unit_get, unit_tp};
    if (pup) p1_correct(&g_rx, now, &g_cc, &ops, &g_last_corr);
    if (now - g_last_log > 10000 && g_rx.st.packets) {
        g_last_log = now;
        char b[600];
        p1_rx_status(&g_rx, b, sizeof b);
        r1_log("puppet: %s", b);
    }
}

/* this PC's own scene flags (CP_SF_LOADING / CUTSCENE / PAUSED / DOWN) of the last pass with the roles bound: 1 = fresh. downed1: the revive takes the raw pad's A only in play
 * (a pause / item menu / cutscene uses A for itself) */
int puppet_scene_now(unsigned *flags)
{
    if (!g_on || !g_roles_bound || GetTickCount() - g_scene_tick > 500u) return 0;
    *flags = g_scene_flags;
    return 1;
}

/* the partner's newest flags (CP_SF_*: the pad packet's while the link is live, else a fresh STATE sample's): 1 = valid. For the downed rule (a lost DOWN / DONE is repaired from them). */
int puppet_partner_flags(unsigned *flags)
{
    if (!g_on || !g_inited) return 0;
    if (g_rx.link == P1_LINK_LIVE) {
        *flags = g_rx.pkt_flags;
        return 1;
    }
    return 0;
}

void puppet_install(void)
{
    char v[8];
    /* lobby-must: ON by default (the product of duo10: the partner's human is driven by his pad records in every network game); it does nothing without a
     * started network session, so the split screen and the solo game are untouched. REV1_PUPPET=0 switches it off (lab A/B). */
    if (!(r1_env("REV1_PUPPET", v, sizeof v) && v[0] == '0')) {
        ensure_init();
        p1_rx_init(&g_rx, &g_rxc, (p1_sink){sink_push, NULL});
        p1_tx_init(&g_tx, &g_txc);
        g_on = 1;
        r1_log("puppet: on (default; REV1_PUPPET=0 switches it off): it binds the roles when a network session plays");
    } else
        r1_log("puppet: off (REV1_PUPPET=0)");
    lt_add_frame_cb(puppet_frame);
}

static void inject_flags_and_rec(const cp_pad_rec *rec)
{
    ensure_init();
    unsigned epoch = g_solo ? 1 : 0;
    lk_t *lk = net1_link_peek();
    if (!g_solo && lk && lk_session(lk)) epoch = cs_epoch(lk_session(lk));
    cp_pad pk;
    memset(&pk, 0, sizeof pk);
    pk.pid = 1, pk.epoch = (uint8_t)epoch, pk.flags = g_inject_flags, pk.seq = ++g_inject_seq, pk.t_ms = GetTickCount(), pk.count = 1, pk.rec[0] = *rec;
    p1_rx_packet(&g_rx, GetTickCount(), &pk, epoch);
}

void puppet_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "", rest[160] = "";
    sscanf(arg ? arg : "", "%15s %159[^\r\n]", sub, rest);
    ensure_init();
    if (!sub[0] || !strcmp(sub, "status")) {
        char a[500], b[300];
        p1_rx_status(&g_rx, a, sizeof a);
        p1_tx_status(&g_tx, b, sizeof b);
        char c[200] = "";
        if (g_last_corr.action) snprintf(c, sizeof c, " | last correction: action %d reason %d err %.1f/%.1f", g_last_corr.action, g_last_corr.reason, g_last_corr.err_xz, g_last_corr.err_y);
        char gd[300], pd[320] = "";
        {
            unsigned gpad = rd32(R1_GAMEPAD_PTR);   /* Pad[k] = sGamePad+0x3C+0x2CC*k: +4 active, +5 assigned, +7 kind (0 unassigned, 2 DirectInput, 4 XInput), +0xC scan flags, +0x10 XInput user */
            for (unsigned k = 0; gpad && k < 2; k++) {
                unsigned pb = gpad + 0x3C + 0x2CC * k;
                size_t n = strlen(pd);
                snprintf(pd + n, sizeof pd - n, "%sPad[%u] kind %u active %u assigned %u user %u scan %x", k ? "; " : "", k, rd8(pb + 7), rd8(pb + 4), rd8(pb + 5), rd32(pb + 0x10), rd32(pb + 0xC));
            }
        }
        char fo[260] = "";
        if (g_foreign_n)
            snprintf(fo, sizeof fo, "; last: (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f) = %.1f units in one pass, pass %u (now %u), %lu ms ago: the ENGINE / a script moved the unit, not the puppet correction", g_foreign_from[0],
                     g_foreign_from[1], g_foreign_from[2], g_foreign_to[0], g_foreign_to[1], g_foreign_to[2], g_foreign_d, g_foreign_frame, g_frames, (unsigned long)(GetTickCount() - g_foreign_ms));
        char ch[600] = "";
        {   /* the chain capture -> Pad[k] raw -> PadData[k] -> the unit: where does the stick stop? */
            unsigned gpad = rd32(R1_GAMEPAD_PTR), pu = unit_by_slot((unsigned)(1 - (g_bound_role < 0 ? 0 : g_bound_role)));
            size_t n = 0;
            if (gpad) {
                n += (size_t)snprintf(ch + n, sizeof ch - n, "sGamePad: process %u current slot %u keyboard mode %u enabled %u", rd8(gpad + 0x20), rd32(gpad + 0x870), rd32(gpad + 0x874), rd8(gpad + 0x882));
                for (unsigned k = 0; k < 2 && n < sizeof ch; k++) {
                    unsigned pb = gpad + 0x3C + 0x2CC * k, pdp = gpad + 0x628 + 0x90 * k;
                    n += (size_t)snprintf(ch + n, sizeof ch - n, "; Pad[%u] raw held %x LX %d LY %d -> PadData[%u] kind %u stick (%.2f, %.2f) right (%.2f, %.2f)", k, rd32(pb + 0x15C), (int)rd32(pb + 0x17C), (int)rd32(pb + 0x180), k, rd32(pdp + 0x10),
                                          rdf(pdp + 0x28), rdf(pdp + 0x2C), rdf(pdp + 0x30), rdf(pdp + 0x34));
                }
            }
            if (pu && n < sizeof ch)
                snprintf(ch + n, sizeof ch - n, "; puppet unit %08x reads stick (%.2f, %.2f) F3C %08x%s F44 %08x ctl %u mode %u (unlock %s, cleared %u times)", pu, rdf(pu + 0x1138), rdf(pu + 0x113C), rd32(pu + 0xF3C),
                         (rd32(pu + 0xF3C) & 0x100000u) ? " = INPUT BAN (bit 20: updatePad zeroes every stick of this unit)" : "", rd32(pu + 0xF44), rd32(rd32(rd32(pu + 0xE80) + 0x38) + 0x34), rd32(pu + 0xF08),
                         g_unlock ? "ON" : "off", g_unlock_n);
        }
        char xi[400] = "", kb[900] = "";
        for (int u = 0; u < 2; u++) {
            unsigned calls = 0, okc = 0, bt = 0, pk = 0, nz = 0;
            int lx = 0, ly = 0;
            testmode_xinput_user(u, &calls, &okc, &bt, &lx, &ly, &pk, &nz);
            size_t n = strlen(xi);
            snprintf(xi + n, sizeof xi - n, "%suser %d: %u polls by the game, %u answered, last state buttons %04x LX %d LY %d packet %u, polls with input %u", u ? "; " : "", u, calls, okc, bt, lx, ly, pk, nz);
        }
        {
            unsigned gpad = rd32(R1_GAMEPAD_PTR), kbo = rd32(0x11E0558u), mo = rd32(0x11E057Cu);
            int n = snprintf(kb, sizeof kb, "keyboard mode %u (previous %u) | keyboard object +0x120 %08x +0x1A0 %08x | mouse object +0x2C %u +0x30 %u +0x34 %u +0x38 %u +0x48 %u | kbskip %d (hook state %d): ",
                             gpad ? rd32(gpad + 0x874) : 0, gpad ? rd32(gpad + 0x878) : 0, kbo ? rd32(kbo + 0x120) : 0, kbo ? rd32(kbo + 0x1A0) : 0, mo ? rd32(mo + 0x2C) : 0, mo ? rd32(mo + 0x30) : 0,
                             mo ? rd32(mo + 0x34) : 0, mo ? rd32(mo + 0x38) : 0, mo ? rd32(mo + 0x48) : 0, g_kbskip, capture_kbskip_state());
            if (n > 0 && (size_t)n < sizeof kb)
                capture_kbskip_text(kb + n, (int)(sizeof kb - (size_t)n));
        }
        int gv = gui_running();
        gui_dump(gd, sizeof gd);
        snprintf(out, (size_t)size, "puppet %s%s, roles %s, frames %u, STATE sent %u | this PC gui_running()=%d (source %s, flags %08x; sources with the bits set from the start and never seen clear, ignored: mask %x), ignorepause %d%s | managers %s | XInput as the game polls it: %s | %s | input chain: %s | game pads: %s (keep user 0: %s%s, rearm %s) | foreign moves of the puppet unit (not our teleport): %u%s | auto %s (%u injected) | %s | %s%s%s%s", g_on ? "ON" : "off",
                 g_solo ? " (solo)" : "", g_roles_bound ? "bound" : "free", g_frames, g_state_sent, gv, g_gui_src == 0 ? "none" : g_gui_src == 100 ? "item menu manager" : g_gui_src == 2 ? "cockpit manager 2 (game over)" : "cockpit manager 3 (pause)",
                 g_gui_flags, g_gui_stuck, g_ignorepause, gv && g_ignorepause ? " (the flag is ignored)" : "", gd, xi, kb, ch, pd, g_keep0 < 0 ? "auto" : g_keep0 ? "on" : "off", g_keep0_on ? " (neutral pad on user 0 active)" : "", g_rearm_on ? "RUNNING" : "off", g_foreign_n, fo, g_auto_on ? "RUNNING" : "off", g_auto_count, a, b, c, g_last_tp[0] ? " | last tp: " : "", g_last_tp);
    } else if (!strcmp(sub, "on")) {
        g_solo = !strcmp(rest, "solo");
        p1_rx_init(&g_rx, &g_rxc, (p1_sink){sink_push, NULL});
        p1_tx_init(&g_tx, &g_txc);
        g_epoch_seen = 0xFFFFFFFFu, g_on = 1;
        snprintf(out, (size_t)size, "puppet ON%s: the partner's pad records drive the second human (pad slot %u), %s", g_solo ? " (solo: records from `net pad inject`)" : "", g_sink_pad,
                 g_solo ? "no session needed" : "starts when the session is PLAYING");
    } else if (!strcmp(sub, "off")) {
        g_on = 0;
        g_solo = 0;
        snprintf(out, (size_t)size, "puppet off (roles are given back at the next pass)");
    } else if (!strcmp(sub, "inject")) {
        char bt[16] = "0";
        int lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        sscanf(rest, "%15s %d %d %d %d %d %d", bt, &lt, &rt, &lx, &ly, &rx, &ry);
        cp_pad_rec r = {(uint16_t)strtoul(bt, NULL, 16), (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry};
        if (!g_on) p1_rx_init(&g_rx, &g_rxc, (p1_sink){sink_push, NULL});
        inject_flags_and_rec(&r);
        snprintf(out, (size_t)size, "packet %u injected: buttons %04x lt %d rt %d L(%d,%d) R(%d,%d)%s", g_inject_seq, r.buttons, r.lt, r.rt, r.lx, r.ly, r.rx, r.ry, g_on ? "" : " (puppet is off: only counted)");
    } else if (!strcmp(sub, "state")) {
        float x = 0, y = 0, z = 0, yaw = 0;
        char fl[16] = "0";
        int n = sscanf(rest, "%f %f %f %f %15s", &x, &y, &z, &yaw, fl);
        if (n < 3) {
            snprintf(out, (size_t)size, "usage: net pad state <x> <y> <z> [yaw] [hex flags]");
            return;
        }
        float p[3] = {x, y, z};
        g_inject_state_flags = (uint16_t)strtoul(fl, NULL, 16);
        lk_t *lk = net1_link_peek();
        cs_t *cs = lk ? lk_session(lk) : NULL;
        p1_rx_state(&g_rx, GetTickCount(), 1, p, yaw, g_inject_state_flags, GetTickCount(), g_solo || !cs ? 1 : cs_epoch(cs));
        snprintf(out, (size_t)size, "partner STATE sample (%.1f, %.1f, %.1f) yaw %.2f flags %04x%s", x, y, z, yaw, g_inject_state_flags, g_solo ? " (solo: kept fresh every pass)" : " (a real session overwrites it with the next sample)");
    } else if (!strcmp(sub, "hold")) {
        g_inject_flags = (uint16_t)strtoul(rest, NULL, 16);
        cp_pad_rec z;
        memset(&z, 0, sizeof z);
        if (g_rx.have_latest) z = g_rx.latest;
        inject_flags_and_rec(&z);
        snprintf(out, (size_t)size, "partner flags now %04x (2048 pause, 512 cutscene, 256 loading, 64 dead, 128 down)", g_inject_flags);
    } else if (!strcmp(sub, "auto")) {
        int ms = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        char bt[16] = "0";
        if (sscanf(rest, "%d %15s %d %d %d %d %d %d", &ms, bt, &lt, &rt, &lx, &ly, &rx, &ry) < 1) {
            snprintf(out, (size_t)size, "usage: net pad auto <ms> <hex buttons> <lt> <rt> <lx> <ly> <rx> <ry>   (auto 0 stops)");
            return;
        }
        if (ms <= 0) {
            g_auto_on = 0;
            snprintf(out, (size_t)size, "auto injection stopped (%u records were injected)", g_auto_count);
            return;
        }
        if (ms > 600000) ms = 600000;
        if (!g_on) p1_rx_init(&g_rx, &g_rxc, (p1_sink){sink_push, NULL});
        g_auto_rec = (cp_pad_rec){(uint16_t)strtoul(bt, NULL, 16), (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry};
        g_auto_count = 0;
        g_auto_until = GetTickCount() + (uint32_t)ms;
        g_auto_on = 1;
        snprintf(out, (size_t)size, "injecting buttons %04x lt %d rt %d L(%d,%d) R(%d,%d) in every frame pass for %d ms%s", g_auto_rec.buttons, lt, rt, lx, ly, rx, ry, ms, g_on ? "" : " (the puppet is off: `net pad on solo` first)");
    } else if (!strcmp(sub, "watch")) {
        int ms = atoi(rest);
        if (ms <= 0) {
            g_watch_until = 0;
            snprintf(out, (size_t)size, "watch off");
            return;
        }
        if (ms > 120000) ms = 120000;
        g_watch_start = GetTickCount(), g_watch_until = g_watch_start + (uint32_t)ms, g_watch_pass = 0;
        snprintf(out, (size_t)size, "watch: every pass in which the puppet unit (slot %d) moves, and one pass in 30, is logged as `puppet-watch:` (position, step, mode, flags, the stick the unit reads at +0x1138, aim, control type) for %d ms; OUR-TP marks the "
                 "puppet's own teleport. Needs `net pad on` (solo) and the unit in the scene", 1 - (g_bound_role < 0 ? 0 : g_bound_role), ms);
    } else if (!strcmp(sub, "kbreset")) {
        /* force pad mode: leave keyboard mode and give the pads their device kind back (the engine does this only on pad 0 activity, once; FUN_00888FD0 / FUN_00888510) */
        unsigned gpad = rd32(R1_GAMEPAD_PTR);
        if (!gpad) {
            snprintf(out, (size_t)size, "net pad kbreset: no sGamePad yet");
            return;
        }
        char b[200] = "";
        wr32(gpad + 0x874, 0);
        wr32(gpad + 0x878, 0);
        for (unsigned k = 0; k < 2; k++) {
            unsigned pdp = gpad + 0x628 + 0x90 * k, kind = rd32(pdp + 0x10), prev = rd32(pdp + 0x14);
            size_t n = strlen(b);
            if (kind == 6) {
                unsigned nk = (prev == 6) ? 0 : prev;
                wr32(pdp + 0x10, nk);
                snprintf(b + n, sizeof b - n, " PadData[%u] kind 6 -> %u;", k, nk);
            } else
                snprintf(b + n, sizeof b - n, " PadData[%u] kind %u kept;", k, kind);
        }
        snprintf(out, (size_t)size, "keyboard mode cleared (sGamePad+0x874/0x878 = 0);%s any keyboard / mouse activity sets it again at once: use `net pad set kbskip 1` to make it irrelevant", b);
    } else if (!strcmp(sub, "rearm")) {
        if (!g_roles_bound) {
            snprintf(out, (size_t)size, "net pad rearm: the roles are not bound yet (net pad on, and PLAYING / solo)");
            return;
        }
        capture_set_source((int)g_sink_pad, CAP_NATIVE);
        g_rearm_until = GetTickCount() + 2500, g_rearm_on = 1;
        snprintf(out, (size_t)size, "pad slot %u is native (disconnected) for 2.5 s, then remote again; the game releases Pad[%u] and assigns it anew within ~1-2 s (net pad status: game pads)", g_sink_pad, g_sink_pad);
    } else if (!strcmp(sub, "stall")) {
        int ms = atoi(rest);
        g_stall_until = GetTickCount() + (uint32_t)(ms > 0 ? ms : 0);
        snprintf(out, (size_t)size, "incoming pad packets are dropped for %d ms", ms);
    } else if (!strcmp(sub, "set")) {
        char k[16] = "";
        int v = 0;
        if (sscanf(rest, "%15s %d", k, &v) != 2) {
            snprintf(out, (size_t)size, "usage: net pad set keepalive|eps|redundancy|soft|hard|jump|neutral|ignorepause|keep0|kbskip|unlock <n>");
            return;
        }
        if (!strcmp(k, "keepalive")) g_txc.keepalive_ms = v, g_tx.cfg.keepalive_ms = v;
        else if (!strcmp(k, "eps")) g_txc.stick_eps = v, g_tx.cfg.stick_eps = v;
        else if (!strcmp(k, "redundancy")) g_txc.redundancy = v < 1 ? 1 : v > CP_PAD_MAX_REC ? CP_PAD_MAX_REC : v, g_tx.cfg.redundancy = g_txc.redundancy;
        else if (!strcmp(k, "soft")) g_cc.soft_xz = (float)v;
        else if (!strcmp(k, "hard")) g_cc.hard_xz = (float)v;
        else if (!strcmp(k, "jump")) g_cc.jump_xz = (float)v;
        else if (!strcmp(k, "neutral")) g_rxc.neutral_ms = v, g_rx.cfg.neutral_ms = v;
        else if (!strcmp(k, "ignorepause")) g_ignorepause = v != 0;
        else if (!strcmp(k, "kbskip")) {
            g_kbskip = v < 0 ? -1 : v != 0;
            if (g_roles_bound) kbskip_bind();   /* effective at once */
        }
        else if (!strcmp(k, "unlock")) g_unlock = v != 0;   /* TEST ONLY */
        else if (!strcmp(k, "keep0")) g_keep0 = v < 0 ? -1 : v != 0;   /* -1 auto (solo only), 0 never, 1 always; takes effect at the next bind (net pad off / on) */
        else {
            snprintf(out, (size_t)size, "net pad set: unknown key '%s'", k);
            return;
        }
        snprintf(out, (size_t)size, "%s = %d", k, v);
    } else
        snprintf(out, (size_t)size, "net pad on [solo] | off | status | inject <hex buttons> <lt> <rt> <lx> <ly> <rx> <ry> | state <x> <y> <z> [yaw] [flags] | hold <hex flags> | auto <ms> <hex> <lt> <rt> <lx> <ly> <rx> <ry> | rearm | kbreset | watch <ms> | stall <ms> | set <key> <n>");
}
