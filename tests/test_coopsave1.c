/*
 * test_coopsave1.c - rev1 live round 15, TASK 5 (native/coopsave1.h): a co-op game has its own save and never writes the solo save.
 * A model of the game's save storage (coopsave1.h FACTS): one file "DATA0.DAT" of 16 sections (here: the profile + 3 slots), proc_Save = read the whole file,
 * replace the pending sections, write it back; proc_Load = read the file, take a slot; the title's Continue item = the slot 0 entry (FUN_008B5D20). The
 * storage calls go through cs1_route like the wrapped ISteamRemoteStorage slots. The pre-fix model (the rule off = REV1_COOPSAVE=0, the round-14 DLL) writes
 * the co-op checkpoints into the solo file: its checks below FAIL as they should.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/coopsave1.h"
#include "../native/secids1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

enum { SEC_PROFILE = 0, SEC_SLOT0, SEC_SLOT1, SEC_SLOT2, SEC_N };
typedef struct { int have; char sec[SEC_N][24]; } file_t;
typedef struct {
    file_t solo, coop;     /* Steam's DATA0.DAT, the mod's COOP0.DAT */
    unsigned solo_writes, solo_reads, coop_writes;
    cs1_t cs;
    int rule;              /* 1 = the fix (the wrapped storage), 0 = the model before it */
} world_t;

/* the wrapped ISteamRemoteStorage slots */
static int st_read(world_t *w, const char *name, file_t *out)
{
    int r = w->rule ? cs1_route(&w->cs, name, 0, w->coop.have) : CS1_STOCK;
    file_t *f = r == CS1_COOP ? &w->coop : &w->solo;
    if (f == &w->solo) w->solo_reads++;
    if (!f->have) return 0;
    *out = *f;
    return 1;
}
static void st_write(world_t *w, const char *name, const file_t *in)
{
    int r = w->rule ? cs1_route(&w->cs, name, 1, w->coop.have) : CS1_STOCK;
    if (r == CS1_COOP) w->coop = *in, w->coop.have = 1, w->coop_writes++;
    else w->solo = *in, w->solo.have = 1, w->solo_writes++;
}
/* BioSaveData's tasks on "DATA0.DAT" */
static void proc_save(world_t *w, int sec, const char *data)
{
    file_t f;
    memset(&f, 0, sizeof f);
    if (!st_read(w, CS1_SOLO_NAME, &f)) memset(&f, 0, sizeof f);   /* no file: a fresh image (proc_Save case 3) */
    snprintf(f.sec[sec], sizeof f.sec[sec], "%s", data);
    st_write(w, CS1_SOLO_NAME, &f);
}
static const char *proc_load(world_t *w, int sec)
{
    static file_t f;
    if (!st_read(w, CS1_SOLO_NAME, &f)) return "";
    return f.sec[sec];
}
/* FUN_008B5D20 with the rule: the slot 0 entry the title's Continue needs */
static int continue_shown(world_t *w, const char *boot_slot0)
{
    if (w->rule && cs1_slots_hidden(&w->cs, w->coop.have)) return 0;
    return boot_slot0[0] != 0;   /* the entries come from the boot read */
}
/* r52's rule (m15c5 LIVE: off as soon as there is no level and no session = at the Episode 2 title card after the END OF EPISODE 1) for the episode-end check */
static int r52_rule;
static void frame(world_t *w, int session, int level, int menu)
{
    if (r52_rule) {
        int on = session || (level && w->cs.active && w->cs.in_coop_level);
        if (session && level) w->cs.in_coop_level = 1;
        if (!level) w->cs.in_coop_level = 0;
        if (on && !w->cs.active) w->cs.ons++;
        if (!on && w->cs.active) w->cs.offs++;
        w->cs.active = on || (session != 0);
        return;
    }
    cs1_step(&w->cs, session, level, menu);
}

/* one evening: solo progress in Episode 3, then a co-op session (New Game -> checkpoints, a game over and its Continue, the session ends INSIDE the level,
 * one more checkpoint there), back at the title, a solo Continue; then a second co-op session that continues the co-op save */
static void evening(int rule, int *solo_same, int *solo_back, int *coop_written, int *coop_cont, int *fresh, int *profile_copied)
{
    world_t w;
    memset(&w, 0, sizeof w);
    cs1_init(&w.cs);
    w.rule = rule;
    w.solo.have = 1;
    snprintf(w.solo.sec[SEC_PROFILE], 24, "profile-solo");
    snprintf(w.solo.sec[SEC_SLOT0], 24, "solo-E3-cp2");
    snprintf(w.solo.sec[SEC_SLOT1], 24, "solo-manual-E2");
    file_t before = w.solo;
    char boot_slot0[24];
    snprintf(boot_slot0, sizeof boot_slot0, "%s", proc_load(&w, SEC_SLOT0));   /* boot: the title reads the solo save */
    frame(&w, 0, 0, 1);
    /* the co-op session STARTs at the title */
    frame(&w, 1, 0, 1);
    *fresh = !continue_shown(&w, boot_slot0);   /* no co-op save yet: New Game only */
    frame(&w, 1, 1, 0);   /* New Game -> 1-1 */
    proc_save(&w, SEC_SLOT0, "coop-1-1-cp1");
    proc_save(&w, SEC_SLOT0, "coop-1-1-cp2");
    const char *go = proc_load(&w, SEC_SLOT0);   /* a game over, the Continue: the checkpoint reload */
    int go_ok = !strcmp(go, "coop-1-1-cp2");
    proc_save(&w, SEC_SLOT0, "coop-1-3-start");
    frame(&w, 0, 1, 0);   /* the host quit: the session ends while this PC still has the level */
    proc_save(&w, SEC_SLOT0, "coop-1-3-cp1");
    frame(&w, 0, 0, 0);   /* the level goes away (loading the title) */
    frame(&w, 0, 0, 1);   /* back at the title menu */
    const char *solo = proc_load(&w, SEC_SLOT0);   /* a solo Campaign > Continue */
    *solo_back = !strcmp(solo, "solo-E3-cp2");
    *solo_same = !memcmp(&w.solo, &before, sizeof before);
    *coop_written = w.coop.have && !strcmp(w.coop.sec[SEC_SLOT0], "coop-1-3-cp1") && go_ok;
    *profile_copied = w.coop.have && !strcmp(w.coop.sec[SEC_PROFILE], "profile-solo") && !strcmp(w.coop.sec[SEC_SLOT1], "solo-manual-E2");
    /* the next co-op session: Continue shown, it continues the co-op save */
    frame(&w, 1, 0, 1);
    int shown = continue_shown(&w, boot_slot0);
    frame(&w, 1, 1, 0);
    *coop_cont = shown && !strcmp(proc_load(&w, SEC_SLOT0), "coop-1-3-cp1");
    frame(&w, 0, 1, 0), frame(&w, 0, 0, 1);
    if (rule) {
        CHECK(w.cs.ons == 2 && w.cs.offs == 2, "B two co-op stays, each switched on at the START and off back at the title");
        CHECK(w.solo_writes == 0, "B the solo file is never written while co-op runs (FileWrite / FileDelete on DATA0.DAT -> the co-op file)");
        CHECK(w.cs.seeded >= 1, "B the first co-op save starts from the solo file READ ONLY (a full image of this build)");
    }
}

/* m15c5 (LIVE): the co-op game reaches the END OF EPISODE 1, the session closes at the Episode 2 title card, Enter goes on into Episode 2 (its first save),
 * later the player returns to the title menu and starts the solo game. r52's rule switched the co-op save off at the title card: Episode 2's save went into
 * the SOLO file (the co-op progress leaked into the solo save). */
static int episode_end(int r52)
{
    world_t w;
    memset(&w, 0, sizeof w);
    cs1_init(&w.cs);
    r52_rule = r52;
    w.rule = 1;
    w.solo.have = 1;
    snprintf(w.solo.sec[SEC_PROFILE], 24, "profile-solo");
    snprintf(w.solo.sec[SEC_SLOT0], 24, "solo-E3-cp2");
    file_t before = w.solo;
    frame(&w, 0, 0, 1), frame(&w, 1, 0, 1), frame(&w, 1, 1, 0);
    proc_save(&w, SEC_SLOT0, "coop-1-3-cp4");
    frame(&w, 1, 0, 0);    /* aEpisodeEnd: the result screen and its save */
    proc_save(&w, SEC_SLOT0, "coop-E2-start");
    frame(&w, 0, 0, 0);    /* the session closes at aEpisodeTitle (the Episode 2 card) */
    frame(&w, 0, 1, 0);    /* Enter: Episode 2 loads, its first checkpoint */
    proc_save(&w, SEC_SLOT0, "E2-cp1-after-coop");
    frame(&w, 0, 0, 0), frame(&w, 0, 0, 1);   /* the pause menu's quit: the title menu */
    const char *solo = proc_load(&w, SEC_SLOT0);
    r52_rule = 0;
    return !memcmp(&w.solo, &before, sizeof before) && !strcmp(solo, "solo-E3-cp2") && w.coop.have && !strcmp(w.coop.sec[SEC_SLOT0], "E2-cp1-after-coop");
}

/* round 16 rule 5 (the lead's decision: split-screen co-op also uses the co-op save): one PC, two humans, no network session. A split game: New Game,
 * checkpoints, a game over + its Continue, back at the title (still armed), the split Continue, then the split disarmed and the solo Continue. Before rule 5
 * (r54) a split game was no co-op game: its checkpoints went into the SOLO file. */
static int split_evening(int rule5, int *cont_ok)
{
    world_t w;
    memset(&w, 0, sizeof w);
    cs1_init(&w.cs);
    w.rule = 1;
    w.solo.have = 1;
    snprintf(w.solo.sec[SEC_PROFILE], 24, "profile-solo");
    snprintf(w.solo.sec[SEC_SLOT0], 24, "solo-E2-cp3");
    file_t before = w.solo;
    char boot_slot0[24];
#define SF(area, menu, level, armed) cs1_step2(&w.cs, 0, rule5 ? cs1_split_now(armed, menu, level) : 0, level, (area) || (menu))   /* area: the title area (press start) */
    SF(1, 0, 0, 1);   /* the press-start screen with the split armed from the settings file: the boot read is the solo one */
    snprintf(boot_slot0, sizeof boot_slot0, "%s", proc_load(&w, SEC_SLOT0));
    int boot_solo = !strcmp(boot_slot0, "solo-E2-cp3") && w.solo_reads == 1;
    SF(1, 1, 0, 1);   /* the title menu, split armed */
    int fresh = !continue_shown(&w, boot_slot0);
    SF(0, 0, 0, 1), SF(0, 0, 1, 1);   /* New Game -> 1-1 */
    proc_save(&w, SEC_SLOT0, "split-1-1-start");
    proc_save(&w, SEC_SLOT0, "split-1-1-4390");
    int go = !strcmp(proc_load(&w, SEC_SLOT0), "split-1-1-4390");   /* a game over, its Continue */
    SF(0, 0, 0, 1), SF(1, 0, 0, 1);   /* back to the title: the press-start screen (the area) */
    SF(1, 1, 0, 1);   /* the title menu, still armed */
    int shown = continue_shown(&w, boot_slot0);
    SF(0, 0, 0, 1), SF(0, 0, 1, 1);
    int cont = shown && !strcmp(proc_load(&w, SEC_SLOT0), "split-1-1-4390");   /* the split Continue */
    SF(0, 0, 0, 1), SF(1, 0, 0, 1), SF(1, 1, 0, 1);
    SF(1, 1, 0, 0);   /* the split disarmed at the title menu */
    int solo_back = !strcmp(proc_load(&w, SEC_SLOT0), "solo-E2-cp3");
#undef SF
    if (cont_ok) *cont_ok = cont && go && fresh && boot_solo;
    return !memcmp(&w.solo, &before, sizeof before) && solo_back && w.solo_writes == 0 && w.coop.have;
}

/* round 16 rule 6 (m15l7 LIVE): each PC's co-op save keeps the bag of the character that PC played; the other character's entry is the remote puppet's
 * (unarmed). Saved with the host as save_host_role, the room now gives the host room_host_role, the host's title Continue. Before rule 6 the roles of the
 * room stay: after a swap each own character comes up with the puppet's (empty) bag. Returns 1 when both own characters have their saved weapons. */
static int role_swap(int rule6, int save_host_role, int room_host_role, int rec_present, int new_game, int *host_role_out)
{
    const char *bag_h[2], *bag_g[2];   /* [unit slot] in each PC's co-op save */
    bag_h[save_host_role] = "host-weapons", bag_h[1 - save_host_role] = "unarmed";
    bag_g[1 - save_host_role] = "guest-weapons", bag_g[save_host_role] = "unarmed";
    cs1_rec rec_h = {CS1_REC_NET, save_host_role};
    char t[64];
    cs1_rec_format(t, sizeof t, &rec_h);   /* the file next to the host's co-op save */
    cs1_rec back;
    int have = rec_present && cs1_rec_parse(t, &back);
    int host_role = room_host_role, why = 0;
    if (rule6 && !new_game) {   /* only the title Continue decide asks the rule */
        int want = cs1_lock_role(1, 1, 1, 1, 1, have ? &back : NULL, host_role, &why);
        if (want != CS1_LK_KEEP) host_role = want;
    }
    if (host_role_out) *host_role_out = host_role;
    if (new_game) return host_role == room_host_role;   /* a New Game: the room's choice, fresh bags */
    return !strcmp(bag_h[host_role], "host-weapons") && !strcmp(bag_g[1 - host_role], "guest-weapons");
}

static void t_round16(void)
{
    int cont = 0, hr = -1;
    CHECK(!split_evening(0, NULL), "G before rule 5 (r54): a split-screen game writes its checkpoints into the SOLO save (the model before the fix fails)");
    CHECK(split_evening(1, &cont), "G rule 5: a split-screen game writes the co-op save; the solo DATA0.DAT byte for byte unchanged; the solo Continue afterwards loads the solo progress");
    CHECK(cont, "G rule 5: the boot read stays solo; the split title shows New Game only without a co-op save; the game over's Continue and the split title Continue read the co-op save");
    CHECK(!role_swap(0, 1, 0, 1, 0, NULL), "H before rule 6 (m15l7 LIVE): saved with the host as Parker, the room in the lead order: an own character comes up unarmed");
    CHECK(!role_swap(0, 0, 1, 1, 0, NULL), "H before rule 6: the other order the same");
    CHECK(role_swap(1, 1, 0, 1, 0, &hr) && hr == 1, "H rule 6: the host's Continue puts the save's roles back (host Parker again): both own characters have their saved weapons");
    CHECK(role_swap(1, 0, 1, 1, 0, &hr) && hr == 0, "H rule 6: the other order: host Jill again, both bags full");
    CHECK(role_swap(1, 1, 1, 1, 0, &hr) && hr == 1, "H rule 6: the room already in the save's order: nothing changes");
    CHECK(role_swap(1, 1, 0, 1, 1, &hr) && hr == 0, "H rule 6: a New Game keeps the room's choice");
    CHECK(!role_swap(1, 1, 0, 0, 0, NULL), "H rule 6: a co-op save without a record (made by r54) cannot be locked (the roles stay)");
    /* the record */
    char t[64];
    cs1_rec r = {CS1_REC_NET, 1}, b;
    CHECK(cs1_rec_format(t, sizeof t, &r) > 0 && !strcmp(t, "mode=net\r\nrole=1\r\n") && cs1_rec_parse(t, &b) && b.mode == CS1_REC_NET && b.role == 1, "I the net record");
    r.mode = CS1_REC_SPLIT, r.role = -1;
    CHECK(cs1_rec_format(t, sizeof t, &r) > 0 && cs1_rec_parse(t, &b) && b.mode == CS1_REC_SPLIT && b.role == -1, "I the split record");
    r.mode = CS1_REC_NONE;
    CHECK(cs1_rec_format(t, sizeof t, &r) == 0 && t[0] == 0, "I no record for nothing");
    r.mode = CS1_REC_NET, r.role = -1;
    CHECK(cs1_rec_format(t, sizeof t, &r) == 0, "I no net record without a role");
    CHECK(!cs1_rec_parse("", &b) && !cs1_rec_parse("mode=net\n", &b) && !cs1_rec_parse("role=1\n", &b) && !cs1_rec_parse("mode=net\nrole=7\n", &b) && !cs1_rec_parse(NULL, &b),
          "I broken records are no records");
    CHECK(cs1_rec_parse("x=1\nrole=0\nmode=net\n", &b) && b.mode == CS1_REC_NET && b.role == 0, "I any order, unknown keys ignored, LF lines");
    cs1_rec last = {CS1_REC_NET, 0}, now = cs1_rec_now(1, 1, 0, &last);
    CHECK(now.mode == CS1_REC_NET && now.role == 1, "I a started session records this PC's role");
    now = cs1_rec_now(0, -1, 1, &last);
    CHECK(now.mode == CS1_REC_SPLIT, "I the split screen records split");
    now = cs1_rec_now(0, -1, 0, &last);
    CHECK(now.mode == CS1_REC_NET && now.role == 0, "I a co-op game going on after its session (Episode 2 after the end) keeps the last record");
    /* the lock rule */
    cs1_rec rn = {CS1_REC_NET, 1}, rs = {CS1_REC_SPLIT, -1};
    int why = -1;
    CHECK(cs1_lock_role(1, 1, 1, 1, 1, &rn, 0, &why) == 1 && why == CS1_LKW_SWAP, "J swap");
    CHECK(cs1_lock_role(1, 1, 1, 1, 1, &rn, 1, &why) == CS1_LK_KEEP && why == CS1_LKW_SAME, "J same");
    CHECK(cs1_lock_role(0, 1, 1, 1, 1, &rn, 0, &why) == CS1_LK_KEEP && why == CS1_LKW_NOT_COOP, "J co-op off (a solo Continue): never");
    CHECK(cs1_lock_role(1, 0, 1, 1, 1, &rn, 0, &why) == CS1_LK_KEEP && why == CS1_LKW_NOT_HOST, "J the guest never decides");
    CHECK(cs1_lock_role(1, 1, 0, 1, 1, &rn, 0, &why) == CS1_LK_KEEP && why == CS1_LKW_NOT_HOST, "J no started session (the split screen): never");
    CHECK(cs1_lock_role(1, 1, 1, 0, 1, &rn, 0, &why) == CS1_LK_KEEP && why == CS1_LKW_PLAYED, "J a level of the session was loaded: never");
    CHECK(cs1_lock_role(1, 1, 1, 1, 0, &rn, 0, &why) == CS1_LK_KEEP && why == CS1_LKW_NO_FILE, "J no co-op file");
    CHECK(cs1_lock_role(1, 1, 1, 1, 1, NULL, 0, &why) == CS1_LK_KEEP && why == CS1_LKW_NO_REC, "J no record");
    CHECK(cs1_lock_role(1, 1, 1, 1, 1, &rs, 0, &why) == CS1_LK_KEEP && why == CS1_LKW_SPLIT, "J a split save: both characters were on one PC");
    /* rule 5 steps */
    cs1_t q;
    cs1_init(&q);
    CHECK(!cs1_split_now(1, 0, 0) && cs1_split_now(1, 1, 0) && cs1_split_now(1, 0, 1) && !cs1_split_now(0, 1, 1), "G split counts at the title menu and in a level, not at press start");
    CHECK(cs1_step2(&q, 0, 0, 0, 1) == 0 && cs1_step2(&q, 0, cs1_split_now(1, 1, 0), 0, 1) == 1, "G armed at the title menu: on");
    CHECK(cs1_step2(&q, 0, 0, 0, 0) == 1 && cs1_step2(&q, 0, 1, 1, 0) == 1, "G loading, the level: on");
    CHECK(cs1_step2(&q, 0, cs1_split_now(1, 0, 0), 0, 1) == 0, "G back at the press-start screen: off (its reads are the solo ones)");
    CHECK(cs1_step2(&q, 0, cs1_split_now(1, 1, 0), 0, 1) == 1 && cs1_step2(&q, 0, cs1_split_now(0, 1, 0), 0, 1) == 0, "G the title menu armed: on; disarmed there: off");
}

static void t_evening(void)
{
    CHECK(!episode_end(1), "F r52's rule (m15c5 LIVE): Episode 2 after the co-op game's end writes the SOLO save (the model before the fix fails)");
    CHECK(episode_end(0), "F the fix: Episode 2 after the co-op game's end goes on in the co-op save; the solo save stays byte for byte; the solo Continue later loads it");
    for (int rule = 0; rule <= 1; rule++) {
        int same, back, written, cont, fresh, prof;
        evening(rule, &same, &back, &written, &cont, &fresh, &prof);
        if (!rule) {
            CHECK(!same && !back, "A before the fix: the co-op checkpoints overwrite the solo save; the solo Continue afterwards loads the CO-OP 1-3 checkpoint");
            CHECK(!fresh, "A before the fix: a co-op start offers the solo save's Continue");
            printf("A before the fix: solo save unchanged %d, solo Continue loads the solo progress %d, co-op start fresh %d\n", same, back, fresh);
        } else {
            CHECK(same, "B the fix: the solo DATA0.DAT byte for byte unchanged after the co-op session (checkpoints, a game over, the end inside a level)");
            CHECK(back, "B the fix: a solo Continue afterwards loads the old solo progress");
            CHECK(written, "B the fix: the co-op save holds the co-op checkpoints, the game over's Continue read the co-op checkpoint");
            CHECK(cont, "B the fix: the next co-op session's Continue continues the co-op save");
            CHECK(fresh, "B the fix: a co-op start without a co-op save is a fresh start (no Continue)");
            CHECK(prof, "B the fix: the co-op save is a full image (the profile, the other slots copied from the solo file at its first write)");
            printf("B the fix: solo save unchanged %d, solo Continue loads the solo progress %d, co-op written %d, co-op Continue %d, fresh start %d\n", same, back, written, cont, fresh);
        }
    }
}

static void t_rules(void)
{
    cs1_t s;
    cs1_init(&s);
    CHECK(cs1_is_solo("DATA0.DAT") && cs1_is_solo("data0.dat") && cs1_is_solo("Data0.Dat"), "C the solo name, any case");
    CHECK(!cs1_is_solo("DATA0.DA") && !cs1_is_solo("DATA0.DAT.bak") && !cs1_is_solo("KEY.DAT") && !cs1_is_solo(NULL) && !cs1_is_solo(""), "C other names are not");
    CHECK(cs1_step(&s, 0, 1, 0) == 0, "C a solo level: off");
    CHECK(cs1_route(&s, "DATA0.DAT", 1, 0) == CS1_STOCK && s.passed == 1, "C off: the stock file");
    CHECK(cs1_step(&s, 1, 0, 1) == 1, "C the session started at the title: on");
    CHECK(cs1_route(&s, "KEY.DAT", 1, 0) == CS1_STOCK, "C on: another file is untouched");
    CHECK(cs1_route(&s, "DATA0.DAT", 1, 0) == CS1_COOP && cs1_route(&s, "DATA0.DAT", 0, 1) == CS1_COOP, "C on: writes always, reads when it exists -> the co-op file");
    CHECK(cs1_route(&s, "DATA0.DAT", 0, 0) == CS1_SEED, "C on, no co-op file: a read is served from the solo file read only");
    CHECK(cs1_slots_hidden(&s, 0) && !cs1_slots_hidden(&s, 1), "C on: the slots read empty without a co-op file only");
    CHECK(cs1_step(&s, 0, 0, 1) == 0 && !cs1_slots_hidden(&s, 0), "C the session ended at the title menu: off");
    CHECK(cs1_step(&s, 0, 1, 0) == 0, "C a solo level after it: off");
    cs1_step(&s, 1, 1, 0), cs1_step(&s, 0, 1, 0);
    CHECK(s.active, "C the session ended inside its level: still on until the title");
    CHECK(cs1_step(&s, 0, 0, 0) == 1, "C m15c5: no level, no title menu (the Episode 2 title card after the END OF EPISODE 1): still on");
    CHECK(cs1_step(&s, 0, 1, 0) == 1, "C m15c5: Episode 2 goes on after the co-op game: still on");
    CHECK(cs1_step(&s, 0, 0, 1) == 0, "C at the title menu: off");
    s.forced = 1;
    CHECK(cs1_step(&s, 0, 0, 1) == 1, "C LAB force on");
    s.forced = 0;
    CHECK(cs1_step(&s, 1, 1, 0) == 0, "C LAB force off");
}

static char *slurp(const char *p, long *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (s) s[n] = 0;
    fclose(f);
    if (len) *len = n;
    return s;
}
static void t_glue(void)
{
    char *g = slurp("../native/coopsave1_game.c", NULL), *p = slurp("../native/probe.c", NULL), *b = slurp("../native/build.sh", NULL), *r = slurp("../native/rolebarrier1_game.c", NULL);
    CHECK(g && strstr(g, "static const int slots[5] = {0, 1, 6, 13, 15};"), "D the wrapped slots: FileWrite 0, FileRead 1, FileDelete 6, FileExists 13, GetFileSize 15");
    CHECK(g && strstr(g, "cs1_route(&CS, name, 1, 0) != CS1_COOP) return ((fw_t)g_orig[0])") && strstr(g, "cs1_route(&CS, name, 1, 0) != CS1_COOP) return ((f1_t)g_orig[6])"),
          "D a write / delete on the solo name in co-op never reaches Steam's DATA0.DAT");
    CHECK(g && strstr(g, "MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)"), "D the co-op file is replaced whole (a temp file first)");
    CHECK(g && strstr(g, "cs_entry_bytes[30] = {0x51, 0x8B, 0x44, 0x24, 0x08, 0x50, 0x8D, 0x54, 0x24, 0x04, 0x52, 0xC7, 0x44, 0x24, 0x08, 0x02,"), "D FUN_008B5D20's bytes in the glue");
    CHECK(g && strstr(g, "REV1_COOPSAVE"), "D REV1_COOPSAVE=0 = the model before the fix");
    CHECK(g && strstr(g, "int at_title = menu_title_age_ms() < 1500u || rolebarrier_title_area();") &&
              strstr(g, "int on = cs1_step2(&CS, session, split, level, at_title) && g_vt != NULL;") && strstr(g, "int session = net1_session_active();"),
          "D m15l9: the title = the title menu page OR the title area (its press-start screen read the CO-OP file 3 times before the menu page was up)");
    char *m = slurp("../native/menu.c", NULL), *k = slurp("../native/lobby_link.c", NULL), *rb = slurp("../native/rolebarrier1_game.c", NULL);
    CHECK(g && strstr(g, "split = cs1_split_now(menu_split_armed(), menu_title_age_ms() < 1500u, level);") && m && strstr(m, "int menu_split_armed(void) { return g_applied_armed == 1; }"),
          "K rule 5: the applied split screen (menu.c) at the title menu / in a level is a co-op game");
    CHECK(g && strstr(g, "#define CS_CONT_DECIDE_RET 0x83DCE2u") && strstr(g, "if (e && (uintptr_t)__builtin_return_address(0) == CS_CONT_DECIDE_RET) cs_continue_decide();"),
          "K rule 6: only the title Continue decide (the call at 0x83DCDD, return 0x83DCE2) with a slot entry asks the lock (New Game / Load keep the room's roles)");
    CHECK(g && strstr(g, "if (ok) cs_rec_write();") && strstr(g, "int r = lk_force_roles(k, want);"), "K rule 6: every co-op write records the roles; the Continue decide puts them back");
    CHECK(k && strstr(k, "r = cs_force_roles(k->s, host_role);") && strstr(k, "if (k->lobby) l1_net_set_role(k->lobby, host_role);"), "K the link: the session's roles and the room's role row follow");
    CHECK(m && strstr(m, "strip_show(L1G_CONT_ROLES, who, 12000);"), "K both PCs show the line when their role changed after the START");
    CHECK(rb && strstr(rb, "if (a.prev >= 0 && a.prev != a.role) {") && strstr(rb, "capture_unbind(a.prev);"), "K the old role's capture is taken back before the new one");
    free(m), free(k), free(rb);
    CHECK(r && strstr(r, "return running_area(an, sizeof an, NULL) && !strcmp(an, \"aRevelationsTitle\");"), "D the title area by the running area's class name");
    CHECK(p && strstr(p, "coopsave_install();") && strstr(p, "\"coopsave\""), "D installed from probe_install, the console command listed");
    CHECK(b && strstr(b, "coopsave1_game.c"), "D in the DLL build");
    CHECK(r && strstr(r, "if (coopsave_active() && !coopsave_file_exists()) return \"this PC has no CO-OP save yet"), "D the guest's title save read is not asked in co-op without a co-op save");
    CHECK(g && strstr(g, "static const unsigned cs_sites[CS_NSITES] = {0x83DA43u, 0x83DA85u, 0x83DCDDu, 0x83A0B9u, 0x83A15Fu, 0x83A2DCu, 0x83A54Cu, 0x83A5C8u};") &&
              strstr(g, "va + 5 + (unsigned)rel == CS_SLOT_ENTRY_VA"),
          "D m15c1 / m15c3: only the title menu's eight calls (the Campaign page + FUN_0083A000's autosave warning; all test NULL) are redirected; FUN_008B5D20 stays stock");
    CHECK(g && !strstr(g, "unsigned char j[5] = {0xE9};"), "D no jump over FUN_008B5D20 any more (r49's crash)");
    long n = 0;
    char *im = slurp("../../image/rerev-image.bin", &n);
    if (im && n > 0xBF9000L) {
        const unsigned char *u = (const unsigned char *)im;
#define AT(va) (u + ((va) - 0x400000L))
        static const unsigned char e[30] = {0x51, 0x8B, 0x44, 0x24, 0x08, 0x50, 0x8D, 0x54, 0x24, 0x04, 0x52, 0xC7, 0x44, 0x24, 0x08, 0x02,
                                            0x00, 0x00, 0x00, 0xE8, 0x78, 0xFF, 0xFF, 0xFF, 0x59, 0xC2, 0x04, 0x00, 0xCC, 0xCC};
        CHECK(!memcmp(AT(0x8B5D20L), e, 30), "E the image: FUN_008B5D20 = type 2 + FUN_008B5CB0");
        static const unsigned char s1[5] = {0xE8, 0xD8, 0x82, 0x07, 0x00}, s2[5] = {0xE8, 0x96, 0x82, 0x07, 0x00}, s3[5] = {0xE8, 0x3E, 0x80, 0x07, 0x00};
        CHECK(!memcmp(AT(0x83DA43L), s1, 5) && !memcmp(AT(0x83DA85L), s2, 5) && !memcmp(AT(0x83DCDDL), s3, 5), "E the image: the title menu's three calls of FUN_008B5D20");
        CHECK(AT(0x83DA48L)[0] == 0x3B && AT(0x83DA8AL)[0] == 0x85 && AT(0x83DCE2L)[0] == 0x85, "E the image: each of the three tests the entry for NULL right after the call");
        {   /* round 16: the call at 0x83DCDD is the Continue decide: a valid entry -> FUN_008B3430(save flow [esi+0x68], slot 0) = the load of slot 0 */
            int32_t rel;
            memcpy(&rel, AT(0x83DCFEL) + 1, 4);
            static const unsigned char ld[5] = {0x8B, 0x4E, 0x68, 0x6A, 0x00};
            CHECK(!memcmp(AT(0x83DCF9L), ld, 5) && AT(0x83DCFEL)[0] == 0xE8 && 0x83DCFEu + 5 + (unsigned)rel == 0x8B3430u,
                  "E the image: after the decide's entry test, mov ecx,[esi+0x68]; push 0; call FUN_008B3430 (the slot 0 load = Continue)");
        }
        static const unsigned va5[5] = {0x83A0B9u, 0x83A15Fu, 0x83A2DCu, 0x83A54Cu, 0x83A5C8u};
        int sites_ok = 1;
        for (int i = 0; i < 5; i++) {
            int32_t rel;
            memcpy(&rel, AT(va5[i]) + 1, 4);
            sites_ok &= AT(va5[i])[0] == 0xE8 && va5[i] + 5 + (unsigned)rel == 0x8B5D20u;
        }
        CHECK(sites_ok, "E the image: FUN_0083A000's five calls of FUN_008B5D20 (the autosave warning of New Game, m15c3)");
        static const unsigned char n1[5] = {0x8B, 0xF8, 0x85, 0xFF, 0x74}, n2[3] = {0x85, 0xC0, 0x74};
        CHECK(!memcmp(AT(0x83A0BEL), n1, 5) && !memcmp(AT(0x83A164L), n2, 3) && !memcmp(AT(0x83A2E1L), n2, 3),
              "E the image: they test the entry for NULL (mov edi, eax; test edi, edi / test eax, eax)");
        static const unsigned char s4[7] = {0xE8, 0xEF, 0x24, 0x08, 0x00, 0x3B, 0xC7};
        CHECK(!memcmp(AT(0x83382CL), s4, 7), "E the image: the caller 0x83382C (left stock: it read [NULL + 0x40] in m15c1)");
        CHECK(!memcmp(AT(0x1097A70L), "DATA0.DAT\0", 10), "E the image: the solo save name at 0x1097A70");
        CHECK(AT(0xBF81E2L)[0] == 0x8B && AT(0xBF81E2L)[1] == 0x13, "E the image: FileWrite = vt[0] (mov edx, [ebx])");
        CHECK(!memcmp(AT(0xBF87C6L), "\x8B\x11\x8B\x52\x04", 5), "E the image: FileRead = vt[1] (+4)");
        CHECK(!memcmp(AT(0xBF8286L), "\x8B\x52\x18", 3), "E the image: FileDelete = vt[6] (+0x18)");
        CHECK(!memcmp(AT(0xBF8747L), "\x8B\x52\x34", 3), "E the image: FileExists = vt[13] (+0x34)");
        CHECK(!memcmp(AT(0xBF8779L), "\x8B\x40\x3C", 3), "E the image: GetFileSize = vt[15] (+0x3C)");
        static const unsigned char nm[5] = {0x68, 0x70, 0x7A, 0x09, 0x01};   /* push 0x1097A70 */
        CHECK(!memcmp(AT(0x8B0651L), nm, 5) && !memcmp(AT(0x8B0965L), nm, 5) && !memcmp(AT(0x8B0E5EL), nm, 5) && !memcmp(AT(0x8B11CBL), nm, 5) && !memcmp(AT(0x8B0452L), nm, 5),
              "E the image: the five name pushes of LoadHeader / Load / Save (2) / Delete");
#undef AT
    } else printf("(no image: the storage bytes are not checked)\n");
    free(g), free(p), free(b), free(r), free(im);
}

/* fix021 (0.2.1): the role record keeps the section of the write (the host's room names the save's own pair), the save panel follows the co-op switch */
static void t_fix021(void)
{
    cs1_rec r = {CS1_REC_NET, 1, 0x91030100u}, q;
    char t[96];
    int n = cs1_rec_format(t, sizeof t, &r);
    CHECK(n > 0 && strstr(t, "role=1") && strstr(t, "area=91030100"), "fix021: the record writes area=");
    CHECK(cs1_rec_parse(t, &q) && q.mode == CS1_REC_NET && q.role == 1 && q.area == 0x91030100u, "fix021: and reads it back");
    CHECK(cs1_rec_parse("mode=net\r\nrole=0\r\n", &q) && q.area == 0, "fix021: a 0.2.0 record has no area (the room keeps its old names)");
    CHECK(cs1_rec_parse("mode=net\nrole=0\narea=9103XX00\n", &q) && q.area == 0, "fix021: a broken area is ignored");
    int l = -1, p = -1;
    CHECK(sec_pair(0x91030100u, &l, &p) && l == 2 && p == 4, "fix021: 3-1 = Parker + Jessica");
    CHECK(sec_pair(0x910A0100u, &l, &p) && l == 2 && p == 4 && sec_pair(0x91020200u, &l, &p) && p == 7, "fix021: 10-1 = Parker + Jessica, 2-2's partner = HUNK");
    CHECK(sec_pair(0x91010100u, &l, &p) && l == 0 && p == 2 && !sec_pair(0x91100200u, &l, &p) && !sec_pair(0x91FF0000u, &l, &p), "fix021: 1-1 = Jill + Parker; 16-2 / none: unknown");
    int pairs = 0;
    for (unsigned e = 1; e <= 0x10; e++)
        for (unsigned ss = 1; ss <= 3; ss++) {
            uint32_t a = 0x91000000u | e << 16 | ss << 8;
            if (sec_kind(a) == RB_SK_PAIR) pairs += sec_pair(a, &l, &p);
        }
    CHECK(pairs == 28, "fix021: every one of the 28 paired sections has its pair");
    FILE *f = fopen("../native/coopsave1_game.c", "rb");
    static char src[200000];
    size_t k = f ? fread(src, 1, sizeof src - 1, f) : 0;
    if (f) fclose(f);
    src[k] = 0;
    CHECK(strstr(src, "cs_panel_pass(at_title, level);") && strstr(src, "sm, 4u, 0u, 6u)") && strstr(src, "sm + 0xB4"), "fix021: the panel's stock slot read at the co-op switch, only with the save task idle");
}

int main(void)
{
    t_fix021();
    t_rules();
    t_evening();
    t_round16();
    t_glue();
    printf("test_coopsave1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
