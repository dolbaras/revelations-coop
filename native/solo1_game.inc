/*
 * solo1_game.inc - included by world1_game.c (after story1_game.inc): the story's solo parts in a network session (ep1-solo, E3 / E9). The decisions are
 * in solo1.h; the evidence in notes\ep1-solo.md.
 *
 *  1. Story view: six FSM actions resolve "the partner" / "main" by the own slot (FsmSetEnablePartner 0x752820, FsmSetPartnerWarp 0x7534E0,
 *     FsmSetPartnerAttackStatus 0x753590, FsmSetPlayerPlaceStatus 0x7536B0, PcsSet/ResetPartnerGenesisSearch 0x776C80 / 0x776D10). Their hooks run the stock
 *     action with the own-slot query answering 0 for this thread only (capture_own_scope): on the PC that plays the partner the story then switches off
 *     the partner (slot 1), not the lead. `world solo scope 0` = the stock view (lab A/B).
 *  2. Per pass (logic thread, model lock held): the two humans' update + draw bits (+0xC 0x400 | 0x800) are followed (s1_unit, the hook's word counts for
 *     the unit it named); the PC whose own human is off watches the other human through camera manager 1 in viewport 0 (s1_watch).
 *  3. Readers for the other modules (any thread, plain volatile copies): solo1_watch_now (menu strip), solo1_unit_off (puppet hold, downed presence),
 *     solo1_cam_gameplay (puppet scene flags).
 */
#include "solo1.h"

#define SOLO_CAMROOT 0x11E00B8u   /* sGameCamera: viewport 0 at +0x30 (camera +4), managers +0xCE0 / +0xCE4 */

typedef unsigned (TC *act2_fn)(void *self, void *a, void *b);
static act2_fn orig_enpart, orig_pwarp, orig_patk, orig_place, orig_gsrch, orig_gsrst;
enum { SO_ENPART = 0, SO_PWARP, SO_PATK, SO_PLACE, SO_GSRCH, SO_GSRST, SO_N };
static const char *const solo_site_names[SO_N] = {"EnablePartner", "PartnerWarp", "PartnerAttack", "PlaceStatus", "SetGenesisSearch", "ResetGenesisSearch"};
static volatile int f_solo_scope = 1;          /* the story's view inside the partner actions (0 = stock own-slot view: lab A/B) */
static volatile LONG solo_calls[SO_N], solo_scoped[SO_N], solo_enpart_logs;
static volatile LONG solo_said_seq;            /* FsmSetEnablePartner ran: the unit it acted on and the value */
static volatile int solo_said_val;
static volatile uint32_t solo_said_unit;
static LONG solo_said_seen;
static s1_unit solo_u[2];                      /* slot 0 / 1 */
static s1_watch solo_w;
static volatile int g_solo_watch;              /* readers' copies */
static volatile uint32_t g_solo_watched, g_solo_enters, g_solo_off_unit[2], g_solo_w_mgr0, g_solo_w_mgr1;
static unsigned solo_write_fail;

static int solo_scope_open(int k)
{
    InterlockedIncrement(&solo_calls[k]);
    int s = f_solo_scope ? s1_story_scope(capture_hooks_ready(), capture_own_effective()) : -1;
    if (s < 0) return 0;
    capture_own_scope(s);
    InterlockedIncrement(&solo_scoped[k]);
    return 1;
}
static void solo_scope_close(int on)
{
    if (on) capture_own_scope(-1);
}

static unsigned TC hook_enpart(void *self, void *a, void *b)
{
    int own = capture_own_effective();
    int sc = solo_scope_open(SO_ENPART);
    unsigned r = orig_enpart(self, a, b);
    solo_scope_close(sc);
    int v = a ? (int)rd8((uint32_t)(uintptr_t)a + 4) : -1;
    int target = sc || own == 0 ? 1 : 0;   /* the slot the action acted on: the story's partner (scoped / the lead's PC), else the stock pick of the partner's PC */
    uint32_t u = 0, other = 0;
    wh_player_by_slot(&MEM, (unsigned)target, &u);
    wh_player_by_slot(&MEM, (unsigned)(1 - target), &other);
    solo_said_unit = u, solo_said_val = v != 0;
    InterlockedIncrement(&solo_said_seq);
    if (InterlockedIncrement(&solo_enpart_logs) <= 40)
        wlog("solo: FsmSetEnablePartner(%d) %s: acted on slot %d unit %08x (flags now %08x); slot %d unit %08x flags %08x; this PC's own slot %d", v,
             sc ? "in the story's view (own slot 0 inside the action)" : own == 0 ? "(own slot 0: the story's view already)" : "in the STOCK view (world solo scope 0)", target, u,
             u ? rd32(u + 0xC) : 0, 1 - target, other, other ? rd32(other + 0xC) : 0, own);
    return r;
}
#define SOLO_WRAP(name, k, orig)                                    \
    static unsigned TC name(void *self, void *a, void *b)          \
    {                                                               \
        int sc = solo_scope_open(k);                                \
        unsigned r = orig(self, a, b);                              \
        solo_scope_close(sc);                                       \
        return r;                                                   \
    }
SOLO_WRAP(hook_pwarp, SO_PWARP, orig_pwarp)
SOLO_WRAP(hook_patk, SO_PATK, orig_patk)
SOLO_WRAP(hook_place, SO_PLACE, orig_place)
SOLO_WRAP(hook_gsrch, SO_GSRCH, orig_gsrch)
SOLO_WRAP(hook_gsrst, SO_GSRST, orig_gsrst)

static uint32_t watchcam_stock_vt_of(uint32_t obj);   /* watch1_game.inc (rev1 live 14): manager 1's vtable as the stock one when it carries the watchcam copy */
static int solo_cam_live(uint32_t m)
{
    if (m < 0x10000u) return 0;
    unsigned st = rd32(m + 0xC) & 7u;
    return st == 1 || st == 2;
}

/* once per pass from world_frame (model lock held) */
static void solo_pass(void)
{
    uint32_t now = GetTickCount();
    uint32_t u[2] = {0, 0};
    wh_player_by_slot(&MEM, 0, &u[0]);
    wh_player_by_slot(&MEM, 1, &u[1]);
    int said = -1;
    uint32_t said_unit = 0;
    LONG seq = solo_said_seq;
    if (seq != solo_said_seen) solo_said_seen = seq, said = solo_said_val ? 0 : 1, said_unit = solo_said_unit;
    for (int k = 0; k < 2; k++) {
        int hs = said >= 0 && u[k] && said_unit == u[k] ? said : -1;
        uint32_t fl = u[k] ? rd32(u[k] + 0xC) : 0;
        if (s1_unit_step(&solo_u[k], u[k], fl, hs, now))
            wlog("solo: the %s (slot %d, unit %08x) is %s here (unit flags %08x)", k ? "partner" : "lead", k, u[k], solo_u[k].off ? "SWITCHED OFF by the story" : "back in play", fl);
        g_solo_off_unit[k] = solo_u[k].off ? u[k] : 0;
    }
    s1_in in;
    memset(&in, 0, sizeof in);
    int own = bound ? bound_own : -1;
    in.in_session = bound && bound_cs && cs_phase(bound_cs) == CP_PHASE_PLAYING && (own == 0 || own == 1);
    if (own == 0 || own == 1) in.own_unit = u[own], in.other_unit = u[1 - own], in.own_off = solo_u[own].off, in.other_off = solo_u[1 - own].off;
    uint32_t root = rd32(SOLO_CAMROOT);
    uint32_t m0 = root ? rd32(root + 0xCE0) : 0, m1 = root ? rd32(root + 0xCE4) : 0;
    if (!solo_cam_live(m0) || !solo_cam_live(m1) || rd32(m0) != watchcam_stock_vt_of(m1)) m0 = m1 = 0;   /* both live managers of the same class (rev1 live 14 m14c3: the
                                                                                                           watchcam's private vtable copy on manager 1 counts as the stock one,
                                                                                                           else the watch ended and restarted every pass) */
    in.mgr0 = m0, in.mgr1 = m1;
    in.vp0 = root ? rd32(root + 0x30 + 4) : 0;
    if (m1) in.mgr1_flags = rd32(m1 + 0xC), in.mgr1_t74 = rd32(m1 + 0x74), in.mgr1_t78 = rd32(m1 + 0x78);
    uint32_t wm1 = solo_w.mgr1, wm0 = solo_w.mgr0;
    s1_out o;
    s1_watch_step(&solo_w, &in, &o);
    uint32_t tm1 = o.left ? wm1 : solo_w.mgr1;   /* the manager the writes are for */
    if (o.entered)
        wlog("solo: WATCH: this PC's human (slot %d, unit %08x) is switched off by the story: viewport 0 shows camera manager 1 (%08x) following the other human (unit %08x) until he is back",
             own, in.own_unit, m1, in.other_unit);
    if (o.set_flags && tm1 && !wr(tm1 + 0xC, &o.flags, 4)) solo_write_fail++;
    if (o.set_tgt && tm1 && (!wr(tm1 + 0x74, &o.t74, 4) || !wr(tm1 + 0x78, &o.t78, 4))) solo_write_fail++;
    if (o.set_vp0 && root && !wr(root + 0x30 + 4, &o.vp0, 4)) solo_write_fail++;
    if (o.left)
        wlog("solo: WATCH ended (%s): %s; manager 1 %s", o.gone ? "the level / its cameras went away" : in.own_unit && !in.own_off ? "this PC's human is back" : "the session / the other human is gone",
             o.set_vp0 ? "viewport 0 shows manager 0 again" : solo_w.back_pending ? "viewport 0 has a cutscene camera now: manager 0 comes back after it" : o.gone ? "nothing written" : "viewport 0 kept",
             o.gone ? "not touched" : "has its old flags and target");
    (void)wm0;
    g_solo_w_mgr0 = solo_w.state == S1_W_ON ? solo_w.mgr0 : 0, g_solo_w_mgr1 = solo_w.state == S1_W_ON ? solo_w.mgr1 : 0;
    g_solo_watched = solo_w.state == S1_W_ON ? solo_w.watched : 0, g_solo_enters = solo_w.enters;
    g_solo_watch = solo_w.state == S1_W_ON;
}

int solo1_watch_now(unsigned *watched, unsigned *enters)
{
    if (watched) *watched = g_solo_watched;
    if (enters) *enters = g_solo_enters;
    return g_solo_watch;
}
int solo1_unit_off(unsigned unit) { return unit && (unit == g_solo_off_unit[0] || unit == g_solo_off_unit[1]); }
int solo1_cam_gameplay(unsigned vp0, unsigned mgr0)
{
    if (vp0 == mgr0) return 1;
    return g_solo_watch && vp0 == g_solo_w_mgr1 && mgr0 == g_solo_w_mgr0;
}

static void solo_status(char *out, int size)
{
    int o = snprintf(out, (size_t)size, "solo: story view %s (sites:", f_solo_scope ? "ON" : "OFF (stock own-slot view)");
    for (int k = 0; k < SO_N && o < size - 60; k++)
        o += snprintf(out + o, (size_t)(size - o), " %s %s %ld/%ld", solo_site_names[k], wh_state_name(H.state[WH_ENPART + k]), (long)solo_scoped[k], (long)solo_calls[k]);
    for (int k = 0; k < 2 && o < size - 120; k++)
        o += snprintf(out + o, (size_t)(size - o), "); slot %d unit %08x flags %08x %s (seen on %d, story said off %d, changes %u", k, solo_u[k].unit, solo_u[k].unit ? rd32(solo_u[k].unit + 0xC) : 0,
                      solo_u[k].off ? "OFF" : "in play", solo_u[k].seen_on, solo_u[k].said_off, solo_u[k].changes);
    if (o < size - 200)
        snprintf(out + o, (size_t)(size - o), "); watch %s%s (manager 1 %08x, watched %08x; enters %u exits %u, viewport sets %u backs %u, target sets %u, cutscene passes %u, write failures %u)",
                 solo_w.state == S1_W_ON ? "ON" : "idle", solo_w.disabled ? " [camera part off: world solo watch 0]" : solo_w.back_pending ? " [manager 0 comes back after the cutscene]" : "",
                 solo_w.mgr1, solo_w.watched, solo_w.enters, solo_w.exits, solo_w.vp_sets, solo_w.vp_backs, solo_w.tgt_sets, solo_w.cutscene_passes, solo_write_fail);
}

/* console: world solo [status] | scope 0|1 | watch 0|1 */
static void solo_cmd(const char *a2, const char *a3, char *out, int size)
{
    if (!strcmp(a2, "scope") && (a3[0] == '0' || a3[0] == '1')) {
        f_solo_scope = a3[0] == '1';
        snprintf(out, (size_t)size, "world solo: the story partner actions run in the %s", f_solo_scope ? "story's view (own slot 0 inside them)" : "STOCK own-slot view (lab A/B)");
    } else if (!strcmp(a2, "watch") && (a3[0] == '0' || a3[0] == '1')) {
        solo_w.disabled = a3[0] == '0';
        snprintf(out, (size_t)size, "world solo: the watch camera is %s", solo_w.disabled ? "OFF (lab)" : "on");
    } else if (!strcmp(a2, "enable") && (a3[0] == '0' || a3[0] == '1')) {
        /* LAB: the story's own action as a script runs it (the stock FsmSetEnablePartner through this hook, on the logic thread): what 13_012_0 / 13_050_0 do */
        if (H.state[WH_ENPART] != WH_ON || !orig_enpart) {
            snprintf(out, (size_t)size, "world solo enable: the FsmSetEnablePartner hook is not ON (%s)", wh_state_name(H.state[WH_ENPART]));
            return;
        }
        uint32_t p0 = 0, p1 = 0;
        wh_player_by_slot(&MEM, 0, &p0), wh_player_by_slot(&MEM, 1, &p1);
        if (!rd32(WH_SPLAYER_PTR) || !p0 || !p1 || !rd32(p0 + 0xFE8) || !rd32(p1 + 0xFE8)) {   /* the stock body asserts on a missing sPlayer / bag (no return) */
            snprintf(out, (size_t)size, "world solo enable: no level with both humans and their bags (sPlayer %08x, units %08x %08x)", rd32(WH_SPLAYER_PTR), p0, p1);
            return;
        }
        uint8_t param[16];
        memset(param, 0, sizeof param);
        param[4] = (uint8_t)(a3[0] == '1');
        LeaveCriticalSection(&mcs);   /* the hook logs and reads only; the model lock is not needed inside the action */
        hook_enpart(NULL, param, NULL);
        EnterCriticalSection(&mcs);
        snprintf(out, (size_t)size, "world solo enable %c: FsmSetEnablePartner(%c) ran through the hook (see the log line `solo: FsmSetEnablePartner`)", a3[0], a3[0]);
    } else
        solo_status(out, size);
}
