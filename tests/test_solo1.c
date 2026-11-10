/*
 * Offline test of native/solo1.h (ep1-solo, E3): the 1-3 solo part in a two-human network game, on a model of the two PCs' engines.
 *   sh tests/build.sh   ->  tests/build/test_solo1.exe
 *
 * The model (facts: notes\ep1-solo.md section 1): per PC two player units (slot 0 Jill, slot 1 Parker) with the unit flags +0xC (state bits 1, update 0x400,
 * draw 0x800), the own-slot query (the host answers 0, the client 1; a thread scope can override it), FsmSetEnablePartner = "the first player unit whose slot
 * is not the own slot" gets 0xC00 cleared / set (0x752820 -> 0x41BC10), camera managers 0 / 1 with flags and a target, viewport 0's camera, the demo scheduler
 * that keeps viewport 0's camera and gives it back after a cutscene (0x891E07), the puppet receiver's hold (this PC's CUTSCENE flag = SILENT: the other
 * human's puppet is not driven), the puppet correction (teleports while not held), the downed presence.
 *
 * The same story is played with the fix (FIX = 1: the story view inside the action, the watch, the scene flags, the holds) and with the model BEFORE the fix
 * (FIX = 0: the stock own-slot view, viewport 0 untouched, "viewport 0 != manager 0" = cutscene, no holds): the second run must fail the solo checks.
 */
#include <stdio.h>
#include <string.h>

#include "../native/solo1.h"

static int fails, checks, quiet;
static int *cur_fail = &fails;
#define CHECK(c) do { checks++; if (!(c)) { (*cur_fail)++; if (!quiet) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- the engine of one PC */
#define ON 0xC01u    /* a live unit: state 1, update + draw */
typedef struct {
    uint32_t addr, flags, t74, t78;
} obj_t;
typedef struct {
    int own;                      /* what capture answers outside any scope (host 0, client 1) */
    int scope;                    /* -1 none, else the scoped answer of the FSM thread */
    obj_t unit[2];                /* slot 0 Jill, slot 1 Parker */
    obj_t mgr0, mgr1, cut;        /* camera managers, the scheduler's cutscene camera */
    uint32_t vp0;                 /* viewport 0's camera */
    uint32_t saved_vp0;           /* the scheduler's kept camera (0 = no cutscene running) */
    int capture_on;
    /* the mod on this PC */
    int fix;
    s1_unit su[2];
    s1_watch w;
    int connected;                /* the partner's PC is in the session */
    /* counters */
    int puppet_driven_passes, puppet_held_passes, tp_on_off_unit, cut_overwritten;
} pc_t;

static int own_answer(const pc_t *p) { return p->scope >= 0 ? p->scope : p->own; }

static void pc_init(pc_t *p, int own, int fix, uint32_t base)
{
    memset(p, 0, sizeof *p);
    p->own = own, p->scope = -1, p->fix = fix, p->capture_on = 1, p->connected = 1;
    p->unit[0] = (obj_t){base + 0x100, ON, 0, 0};
    p->unit[1] = (obj_t){base + 0x200, ON, 0, 0};
    p->mgr0 = (obj_t){base + 0x300, 0xF4C01u, base + 0x100 + 0x100u * (unsigned)own, 0};
    p->mgr0.t78 = p->mgr0.t74;
    p->mgr1 = (obj_t){base + 0x400, 0xF4001u, 0, 0};   /* the stock game leaves manager 1 without 0xC00 (stage5.md S2) */
    p->cut = (obj_t){base + 0x500, 0xC01u, 0, 0};
    p->vp0 = p->mgr0.addr;
}

/* FsmSetEnablePartner as the engine runs it (0x752820): the partner = the first player unit whose slot is not the own slot (0x41BC10) */
static void stock_enable_partner(pc_t *p, int v)
{
    int own = own_answer(p);
    for (int k = 0; k < 2; k++)
        if (k != own) {
            if (v) p->unit[k].flags |= S1_UNIT_ON;
            else p->unit[k].flags &= ~S1_UNIT_ON;
            return;
        }
}
static int hook_said_seq, hook_said_val, hook_said_slot[2];
/* the hook (solo1_game.inc hook_enpart): the story view when needed, then the stock action; the hook names the unit it acted on */
static void fsm_enable_partner(pc_t *p, int v)
{
    int sc = p->fix ? s1_story_scope(p->capture_on, p->own) : -1;
    if (sc >= 0) p->scope = sc;
    int target = own_answer(p) == 0 ? 1 : 0;
    stock_enable_partner(p, v);
    p->scope = -1;
    hook_said_seq++, hook_said_val = v, hook_said_slot[p->own] = target;
    if (p->fix) {   /* the glue hands the hook's word (the unit it acted on) to the unit rule in its next pass */
        uint32_t fl = p->unit[target].flags;
        s1_unit_step(&p->su[target], p->unit[target].addr, fl, v ? 0 : 1, 0);
    }
}

/* the demo scheduler: a cutscene puts its camera into viewport 0 and gives back the camera it found */
static void cutscene_begin(pc_t *p) { p->saved_vp0 = p->vp0, p->vp0 = p->cut.addr; }
static void cutscene_end(pc_t *p) { p->vp0 = p->saved_vp0, p->saved_vp0 = 0; }

/* the level goes away and comes back (a Continue / checkpoint): new units, new managers */
static void reload(pc_t *p, uint32_t base)
{
    int own = p->own, fix = p->fix;
    s1_watch w = p->w;
    s1_unit su0 = p->su[0], su1 = p->su[1];
    pc_init(p, own, fix, base);
    p->w = w, p->su[0] = su0, p->su[1] = su1;
}

static obj_t *obj_at(pc_t *p, uint32_t a)
{
    obj_t *all[6] = {&p->unit[0], &p->unit[1], &p->mgr0, &p->mgr1, &p->cut, NULL};
    for (int i = 0; all[i]; i++)
        if (all[i]->addr == a) return all[i];
    return NULL;
}

/* one logic pass of the mod on this PC: what solo1_game.inc (watch), puppet_game.c (scene flags, hold) and downed1.c (presence) do */
static void pass(pc_t *p, uint32_t now, int *cutscene_flag, int *present)
{
    int other = 1 - p->own;
    if (p->fix) {
        for (int k = 0; k < 2; k++) s1_unit_step(&p->su[k], p->unit[k].addr, p->unit[k].flags, -1, now);
        s1_in in;
        memset(&in, 0, sizeof in);
        in.in_session = 1;
        in.own_unit = p->unit[p->own].addr, in.other_unit = p->unit[other].addr;
        in.own_off = p->su[p->own].off, in.other_off = p->su[other].off;
        in.mgr0 = p->mgr0.addr, in.mgr1 = p->mgr1.addr, in.vp0 = p->vp0;
        in.mgr1_flags = p->mgr1.flags, in.mgr1_t74 = p->mgr1.t74, in.mgr1_t78 = p->mgr1.t78;
        s1_out o;
        uint32_t was_vp0 = p->vp0;
        s1_watch_step(&p->w, &in, &o);
        obj_t *m1 = obj_at(p, p->w.mgr1);
        if (o.set_flags && m1) m1->flags = o.flags;
        if (o.set_tgt && m1) m1->t74 = o.t74, m1->t78 = o.t78;
        if (o.set_vp0) {
            if (was_vp0 == p->cut.addr) p->cut_overwritten++;
            p->vp0 = o.vp0;
        }
    }
    /* puppet_game.c scene_flags: the cutscene flag of THIS PC */
    int cut = p->fix ? !s1_cam_gameplay(&p->w, p->vp0, p->mgr0.addr) : p->vp0 != p->mgr0.addr;
    if (cutscene_flag) *cutscene_flag = cut;
    /* the receiver of the other human's puppet: held while this PC is in a cutscene, or (fix) while the story switched the puppet off here */
    int pup_off = p->fix ? p->su[other].off : 0;
    uint16_t hold = s1_puppet_hold(cut ? 0x200 : 0, pup_off, 0x200);
    if (!p->fix) hold = cut ? 0x200 : 0;
    if (hold) p->puppet_held_passes++;
    else {
        p->puppet_driven_passes++;
        if ((p->unit[other].flags & S1_UNIT_ON) != S1_UNIT_ON) p->tp_on_off_unit++;   /* the correction writes a unit the story switched off */
    }
    /* downed1.c presence of the partner */
    if (present) *present = p->fix ? s1_partner_present(p->connected, p->su[other].off) : p->connected;
}

/* what the human in front of this PC sees: the unit viewport 0 follows, drawn or not */
static uint32_t view_target(pc_t *p)
{
    obj_t *c = obj_at(p, p->vp0);
    if (!c || c == &p->cut) return 0;
    return c->t74;
}
static int view_ok(pc_t *p, int slot)   /* viewport 0 follows this character, the character is drawn and updated, its camera updates */
{
    obj_t *c = obj_at(p, p->vp0);
    return c && c != &p->cut && c->t74 == p->unit[slot].addr && (c->flags & S1_UNIT_ON) == S1_UNIT_ON && (p->unit[slot].flags & S1_UNIT_ON) == S1_UNIT_ON;
}

/* ---------------------------------------------------------------- the story of 1-3 on two PCs (roles: the host plays `host_slot`) */
static int story_run(int fix, int host_slot, int verbose)
{
    int before = *cur_fail;
    pc_t H, C;
    pc_init(&H, host_slot, fix, 0x10000000u);
    pc_init(&C, 1 - host_slot, fix, 0x20000000u);
    pc_t *lead_pc = host_slot == 0 ? &H : &C, *part_pc = host_slot == 0 ? &C : &H;   /* the PC of Jill / of Parker */
    uint32_t t = 1000;
    int cut, pres;
    /* 13-1 .. 13-3: both in play, nobody watches */
    for (int i = 0; i < 5; i++, t += 33) pass(&H, t, &cut, &pres), pass(&C, t, &cut, &pres);
    CHECK(H.w.state == S1_W_IDLE && C.w.state == S1_W_IDLE && view_ok(lead_pc, 0) && view_ok(part_pc, 1));
    /* SF[4943]: 13_012_0_0_main n11 on the lead's PC, 300 ms later on the partner's PC (the flag mirror) */
    fsm_enable_partner(lead_pc, 0);
    for (int i = 0; i < 9; i++, t += 33) pass(&H, t, &cut, &pres), pass(&C, t, &cut, &pres);
    fsm_enable_partner(part_pc, 0);
    for (int i = 0; i < 10; i++, t += 33) pass(&H, t, &cut, &pres), pass(&C, t, &cut, &pres);
    /* the story's world on both PCs: Parker off, Jill on */
    CHECK((H.unit[1].flags & S1_UNIT_ON) == 0 && (C.unit[1].flags & S1_UNIT_ON) == 0);
    CHECK((H.unit[0].flags & S1_UNIT_ON) == S1_UNIT_ON && (C.unit[0].flags & S1_UNIT_ON) == S1_UNIT_ON);   /* stock: the partner's PC switched JILL off */
    /* the partner's human watches Jill; the lead's human still plays */
    CHECK(view_ok(part_pc, 0));
    CHECK(view_ok(lead_pc, 0) && lead_pc->w.state == S1_W_IDLE);
    /* the partner's PC drives the Jill puppet (the watch is no cutscene there), the lead's PC holds the switched-off Parker puppet */
    int drv0 = part_pc->puppet_driven_passes, held0 = lead_pc->puppet_held_passes, tp0 = lead_pc->tp_on_off_unit;
    pass(part_pc, t, &cut, &pres);
    CHECK(!cut && part_pc->puppet_driven_passes == drv0 + 1);
    pass(lead_pc, t, &cut, &pres);
    CHECK(lead_pc->puppet_held_passes == held0 + 1 && lead_pc->tp_on_off_unit == tp0);
    /* downed: on the lead's PC the switched-off partner is not present (a lethal hit = an ordinary death, nobody waits for a revive) */
    CHECK(pres == 0);
    pass(part_pc, t, &cut, &pres);
    CHECK(pres == 1);   /* the partner's PC: the lead is there */
    t += 33;
    /* 13-5 a cutscene (the tied man) on the partner's PC: the scheduler's camera is never replaced, the watch comes back after it */
    cutscene_begin(part_pc);
    for (int i = 0; i < 20; i++, t += 33) pass(part_pc, t, &cut, &pres);
    CHECK(cut && part_pc->vp0 == part_pc->cut.addr && part_pc->cut_overwritten == 0);
    cutscene_end(part_pc);
    for (int i = 0; i < 3; i++, t += 33) pass(part_pc, t, &cut, &pres);
    CHECK(!cut && view_ok(part_pc, 0));
    /* a death in the solo part, Continue from the checkpoint 257/1 (SF[4899]): the level is rebuilt (new units and managers), 13_00_event_must_00 n6 switches
     * Parker off again at the load: the watch starts again on the new instance, nothing is written into the freed objects */
    reload(&H, 0x30000000u), reload(&C, 0x40000000u);
    for (int i = 0; i < 4; i++, t += 33) pass(&H, t, &cut, &pres), pass(&C, t, &cut, &pres);
    CHECK(part_pc->w.state == S1_W_IDLE && view_ok(part_pc, 1) && part_pc->cut_overwritten == 0);
    fsm_enable_partner(lead_pc, 0), fsm_enable_partner(part_pc, 0);
    for (int i = 0; i < 4; i++, t += 33) pass(&H, t, &cut, &pres), pass(&C, t, &cut, &pres);
    CHECK(view_ok(part_pc, 0) && view_ok(lead_pc, 0));
    /* 13_050_0: FsmSetEnablePartner(1) under a running PCS scene on the partner's PC: the watch ends, viewport 0 gets manager 0 when the scene hands its camera back */
    cutscene_begin(part_pc);
    fsm_enable_partner(lead_pc, 1), fsm_enable_partner(part_pc, 1);
    for (int i = 0; i < 3; i++, t += 33) pass(&H, t, &cut, &pres), pass(&C, t, &cut, &pres);
    CHECK(part_pc->vp0 == part_pc->cut.addr && part_pc->cut_overwritten == 0);
    cutscene_end(part_pc);
    for (int i = 0; i < 3; i++, t += 33) pass(&H, t, &cut, &pres), pass(&C, t, &cut, &pres);
    /* both humans play again, each on his own camera; manager 1 has its stock flags and target back */
    CHECK(view_ok(lead_pc, 0) && view_ok(part_pc, 1));
    CHECK(part_pc->w.state == S1_W_IDLE && (part_pc->mgr1.flags & S1_UNIT_ON) == 0 && part_pc->mgr1.t74 == 0);
    CHECK((H.unit[1].flags & S1_UNIT_ON) == S1_UNIT_ON && (C.unit[1].flags & S1_UNIT_ON) == S1_UNIT_ON && (H.unit[0].flags & S1_UNIT_ON) == S1_UNIT_ON && (C.unit[0].flags & S1_UNIT_ON) == S1_UNIT_ON);
    pass(lead_pc, t, &cut, &pres);
    CHECK(pres == 1);
    /* a second watch without a cutscene ends with viewport 0 = manager 0 at once */
    fsm_enable_partner(part_pc, 0);
    for (int i = 0; i < 3; i++, t += 33) pass(part_pc, t, &cut, &pres);
    CHECK(view_ok(part_pc, 0));
    fsm_enable_partner(part_pc, 1);
    pass(part_pc, t, &cut, &pres);
    CHECK(view_ok(part_pc, 1) && part_pc->vp0 == part_pc->mgr0.addr);
    if (verbose) printf("  story (fix %d, host plays slot %d): %d failed checks; watch enters %u exits %u, viewport sets %u backs %u, cutscene passes %u\n", fix, host_slot, *cur_fail - before,
                        part_pc->w.enters, part_pc->w.exits, part_pc->w.vp_sets, part_pc->w.vp_backs, part_pc->w.cutscene_passes);
    return *cur_fail - before;
}

static void unit_rules(void)
{
    s1_unit u;
    s1_unit_reset(&u);
    /* a unit never seen on and not named by the story is not off (a unit being built) */
    CHECK(s1_unit_step(&u, 0x100, 0x001, -1, 1) == 0 && !u.off);
    /* seen on, then the bits clear: off; on again: back */
    CHECK(s1_unit_step(&u, 0x100, ON, -1, 2) == 0 && !u.off && u.seen_on);
    CHECK(s1_unit_step(&u, 0x100, 0x001, -1, 3) == 1 && u.off);
    CHECK(s1_unit_step(&u, 0x100, ON, -1, 4) == 1 && !u.off);
    /* the story's word counts for a unit never seen on (a Continue past SF[4943]: switched off at the load) */
    s1_unit_reset(&u);
    CHECK(s1_unit_step(&u, 0x200, 0x001, 1, 5) == 1 && u.off);
    /* a new unit pointer starts over */
    CHECK(s1_unit_step(&u, 0x300, 0x001, -1, 6) == 1 && !u.off);
    /* ep1-probe e1p1 / e1p2 live: an examine motion clears the draw bit alone (+0xC 180f4452: 0x400 set, 0x800 clear) - not the story's switch-off (the action
     * clears both): not off. The old rule (one bit missing = off) is modelled below and must call it off. Only the update bit gone: not off either. */
    s1_unit_reset(&u);
    s1_unit_step(&u, 0x400, ON, -1, 7);
    CHECK(s1_unit_step(&u, 0x400, 0x180f4452u, -1, 8) == 0 && !u.off);
    CHECK(s1_unit_step(&u, 0x400, 0x200f4852u, -1, 9) == 0 && !u.off);
    CHECK(s1_unit_step(&u, 0x400, 0x180f4052u, -1, 10) == 1 && u.off);   /* both clear (the action): off */
    {
        int old_on = (0x180f4452u & S1_UNIT_ON) == S1_UNIT_ON, old_off = !old_on;   /* the rule before the fix, seen on before */
        CHECK(old_off == 1);
        printf("  unit rule: the draw bit alone clear (examine motion): the rule before the fix calls it off (%d), the fix does not\n", old_off);
    }
    /* the story's word with one bit still set (a hook seen before the flags settle): off */
    s1_unit_reset(&u);
    CHECK(s1_unit_step(&u, 0x500, 0x401, 1, 11) == 1 && u.off);
    /* the story view: needed only on a PC whose own slot answers not 0 with capture on */
    CHECK(s1_story_scope(1, 1) == 0 && s1_story_scope(1, 0) == -1 && s1_story_scope(0, 1) == -1);
    /* presence / hold helpers */
    CHECK(s1_partner_present(1, 0) == 1 && s1_partner_present(1, 1) == 0 && s1_partner_present(0, 0) == 0);
    CHECK(s1_puppet_hold(0, 1, 0x200) == 0x200 && s1_puppet_hold(0x800, 0, 0x200) == 0x800 && s1_puppet_hold(0x800, 1, 0x200) == 0xA00);
    /* the watch never starts without both live managers, or when the other human is off too, or when the camera part is switched off */
    s1_watch w;
    memset(&w, 0, sizeof w);
    s1_in in = {1, 0x100, 0x200, 1, 0, 0x300, 0x400, 0x300, 0xF4001, 0, 0};
    s1_out o;
    in.mgr1 = 0;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_IDLE && !o.set_vp0);
    in.mgr1 = 0x400, in.other_off = 1;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_IDLE && !o.set_vp0);
    in.other_off = 0, w.disabled = 1;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_IDLE && !o.set_vp0);
    w.disabled = 0, in.in_session = 0;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_IDLE);
    in.in_session = 1;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_ON && o.entered && o.set_vp0 && o.vp0 == 0x400 && o.set_flags && (o.flags & S1_UNIT_ON) == S1_UNIT_ON && o.set_tgt && o.t74 == 0x200 && o.t78 == 0x200);
    CHECK(w.sv_flags == 0xF4001 && w.sv_t74 == 0 && s1_cam_gameplay(&w, 0x400, 0x300) && s1_cam_gameplay(&w, 0x300, 0x300) && !s1_cam_gameplay(&w, 0x500, 0x300));
    /* the session ends during the watch: everything back */
    in.vp0 = 0x400, in.mgr1_flags = 0xF4C01, in.mgr1_t74 = in.mgr1_t78 = 0x200, in.in_session = 0;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_IDLE && o.left && o.set_vp0 && o.vp0 == 0x300 && o.set_flags && o.flags == 0xF4001 && o.set_tgt && o.t74 == 0 && !s1_cam_gameplay(&w, 0x400, 0x300));
    /* the managers are gone (a load) during the watch: no write at all */
    in.in_session = 1, in.vp0 = 0x300, in.mgr1_flags = 0xF4001, in.mgr1_t74 = in.mgr1_t78 = 0;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_ON);
    in.mgr0 = 0x900, in.mgr1 = 0xA00, in.vp0 = 0x900;
    s1_watch_step(&w, &in, &o);
    CHECK(w.state == S1_W_IDLE && o.left && o.gone && !o.set_vp0 && !o.set_flags && !o.set_tgt);
}

/* the glue is wired where the model says (source text of the game modules) */
static char *slurp(const char *path)
{
    static char bufs[6][400000];
    static int next;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = bufs[next++ % 6];
    size_t n = fread(b, 1, sizeof bufs[0] - 1, f);
    fclose(f);
    /* drop CRs: a worktree checked out with core.autocrlf=true has CRLF sources, the checks below are written with "\n" */
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (b[i] != '\r') b[k++] = b[i];
    b[k] = 0;
    return b;
}
static void wiring(void)
{
    char *w = slurp("../native/world1_game.c");
    CHECK(w && strstr(w, "#include \"solo1_game.inc\""));
    CHECK(w && strstr(w, "{WH_ENPART, (void *)hook_enpart, (void **)&orig_enpart}") && strstr(w, "{WH_GSRST, (void *)hook_gsrst, (void **)&orig_gsrst}"));
    const char *wf = w ? strstr(w, "static void world_frame(void)") : NULL;
    CHECK(wf && strstr(wf, "solo_pass();") && strstr(wf, "solo_pass();") < strstr(wf, "LeaveCriticalSection(&mcs);\n}"));
    char *g = slurp("../native/solo1_game.inc");
    const char *he = g ? strstr(g, "static unsigned TC hook_enpart(") : NULL;
    CHECK(he && strstr(he, "solo_scope_open(SO_ENPART)") && strstr(he, "orig_enpart(self, a, b)") && strstr(he, "solo_scope_close(sc)"));
    CHECK(g && strstr(g, "s1_story_scope(capture_hooks_ready(), capture_own_effective())") && strstr(g, "capture_own_scope(s);") && strstr(g, "capture_own_scope(-1);"));
    char *pg = slurp("../native/puppet_game.c");
    CHECK(pg && strstr(pg, "!solo1_cam_gameplay(rd32(root + 0x30 + 4), mgr0)) f |= CP_SF_CUTSCENE;"));
    CHECK(pg && strstr(pg, "s1_puppet_hold((uint16_t)(flags & (CP_SF_LOADING | CP_SF_PAUSED | CP_SF_CUTSCENE)), pup_off, CP_SF_CUTSCENE)"));
    char *dn = slurp("../native/downed1.c");
    CHECK(dn && strstr(dn, "present = s1_partner_present(present, off);"));
    char *sp = slurp("../native/split.c");
    CHECK(sp && strstr(sp, "int solo = !other_unit || other_s1.off;"));
    char *mn = slurp("../native/menu.c");
    CHECK(mn && strstr(mn, "strip_show(L1G_SOLO_WATCH, who, 12000);"));
}

int main(void)
{
    unit_rules();
    wiring();
    /* the fix, both role choices (the host plays Jill = the tested default; the host plays Parker = R7) */
    int f1 = story_run(1, 0, 1);
    int f2 = story_run(1, 1, 1);
    CHECK(f1 == 0 && f2 == 0);
    /* the model before the fix: the same story must fail (the partner's PC switches Jill off and keeps its own Parker; nobody watches; no holds) */
    int fake = 0;
    cur_fail = &fake, quiet = 1;
    int b1 = story_run(0, 0, 0);
    int b2 = story_run(0, 1, 0);
    cur_fail = &fails, quiet = 0;
    printf("  the model before the fix fails %d of the story checks (host plays Jill), %d (host plays Parker)\n", b1, b2);
    CHECK(b1 >= 6 && b2 >= 6);
    printf("test_solo1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
