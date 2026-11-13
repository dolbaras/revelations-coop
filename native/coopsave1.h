/*
 * coopsave1.h - rev1 live round 15, TASK 5 (the owner's decision 2026-11-09): a co-op game has its OWN save, separate from the player's solo campaign save; a
 * co-op game never writes the solo save. Pure (no Windows, no engine): coopsave1_game.c runs it, tests/test_coopsave1.c drives it.
 *
 * FACTS (static, Ghidra r15 o16-o19 + capstone on the build 1573b79e):
 *   - the whole campaign save is ONE Steam Cloud file "DATA0.DAT" (0x1097A70), handled by BioSaveData (prog\game\save\dependent\BioSaveData_Win.cpp): its
 *     four tasks proc_LoadHeader 0x8B05C0, proc_Load 0x8B0860, proc_Save 0x8B0D50, proc_Delete 0x8B03C0 (queued by the dispatcher at 0x8B13C2: a missing
 *     header (section 0 size [+0x70] == 0) queues proc_LoadHeader first) all name the file with FUN_00BF7E80(storage [0x11E253C], "DATA0.DAT") (strncpy to
 *     storage + 0xD44) and start the storage op FUN_00BF8B20;
 *   - the storage worker calls the game's ISteamRemoteStorage (CSteamAPIContext +0x24 = [9]): vt[0] FileWrite (0xBF81E2), vt[1] FileRead (+4), vt[6] FileDelete
 *     (+0x18, 0xBF8286), vt[13] FileExists (+0x34, 0xBF8747), vt[15] GetFileSize (+0x3C, 0xBF8779);
 *   - proc_Save reads the existing file first (magic 'BHDR' 0x52444842 version 5), replaces only the pending sections at their table offsets and writes the
 *     whole file back: every save, checkpoint or slot, rewrites DATA0.DAT; the layout (16 sections, the defaults table 0x1097A8C) is the same for any file
 *     of this build;
 *   - the title's Continue / Load items and the save flow ask the slot entries through FUN_008B5D20(sm, slot) (15 callers: the title menu 0x83DA43 /
 *     0x83DA85 / 0x83DCDD, the load / save pages, the save flow 0x8B274F / 0x8B2CC8 / 0x8B3B76); the entries come from the file read at boot.
 * THE RULE:
 *   1. co-op is ON from the network session's START (the title, both PCs) and stays on until this PC is back at the MAIN TITLE MENU with no session (a session
 *      that ends inside a level: its last checkpoints still go to the co-op save; m15c5 LIVE: after the END OF EPISODE 1 the session closes at aEpisodeTitle
 *      (the Episode 2 title card, no level, not the title menu) and Enter goes on into Episode 2 with the co-op progress: that game is the co-op game going on,
 *      its saves go to the co-op save too, never to the solo one; m15l9 LIVE: "the title" is the title AREA (aRevelationsTitle), its press-start screen
 *      already reads the save before the title menu page is up);
 *   2. while co-op is on, every WRITE / DELETE on the solo file name ("DATA0.DAT", any case) goes to the co-op file (a local file of the mod:
 *      %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS\rev1_coop_save\COOP0.DAT, never in Steam Cloud); READ / SIZE / EXISTS go to the co-op file when it
 *      exists, else to the solo file READ-ONLY (CS1_SEED): proc_Save's read-modify-write then starts the co-op file as a full image of this build (the profile
 *      section, the other slots) with the co-op sections in it. The solo file is never written or deleted by a co-op game;
 *   3. while co-op is on and the co-op file does not exist, every slot entry reads as empty (FUN_008B5D20 -> 0): the title shows New Game only, a co-op start
 *      without a co-op save is a fresh start, the guest's own save read at the title is not asked (the direct follow);
 *   4. with co-op off nothing changes (the stock calls, the solo file).
 * ROUND 16 (the lead's decisions 2026-11-09):
 *   5. SPLIT SCREEN co-op (two humans on one PC) uses the co-op save too: the split armed (menu.c applied it) counts like the session while the title MENU is up
 *      (the Campaign page's Continue is the co-op save's) and while a level runs; at the press-start screen it does not (the boot read stays the solo one);
 *      from then on rule 1 (on until the title) as for a network game (cs1_split_now);
 *   6. CONTINUE KEEPS THE ROLES OF THE SAVE (m15l7 LIVE: a role swap between the save and the Continue left the guest's Parker unarmed: the co-op save keeps the
 *      bag of the character that PC played, the other character's entry is the remote puppet's): every write of the co-op file records next to it (COOP0.INF)
 *      how this PC played: "net" + its role (0 lead / 1 partner) in a network game, "split" in a split-screen game (both characters on this PC: nothing to
 *      lock). The host's title Continue of a co-op save recorded as "net" puts the roles back to the recorded ones before the save is read (cs1_lock_role:
 *      the session's roles change once, before the first level of the session; both PCs show the line); a New Game keeps the room's choice.
 */
#ifndef COOPSAVE1_H
#define COOPSAVE1_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CS1_SOLO_NAME "DATA0.DAT"
#define CS1_DIR "rev1_coop_save"
#define CS1_FILE "COOP0.DAT"
#define CS1_REC_FILE "COOP0.INF"   /* rule 6: the role record next to the co-op save */

/* rule 6: how this PC played when the co-op file was last written */
enum { CS1_REC_NONE = 0, CS1_REC_NET = 1, CS1_REC_SPLIT = 2 };
typedef struct { int mode, role; uint32_t area; } cs1_rec;   /* role: 0 lead / 1 partner for CS1_REC_NET, else -1; area (fix021): the section of the write, 0 = unknown */

/* "mode=net\r\nrole=1\r\n" / "mode=split\r\n"; returns the length, 0 for a record that says nothing (CS1_REC_NONE, a net record without a role) */
static inline int cs1_rec_format(char *out, size_t size, const cs1_rec *r)
{
    int n = 0;
    if (!out || !size) return 0;
    out[0] = 0;
    if (r && r->mode == CS1_REC_NET && (r->role == 0 || r->role == 1)) n = snprintf(out, size, "mode=net\r\nrole=%d\r\n", r->role);
    else if (r && r->mode == CS1_REC_SPLIT) n = snprintf(out, size, "mode=split\r\n");
    if (n > 0 && (size_t)n < size && r->area) n += snprintf(out + n, size - (size_t)n, "area=%08X\r\n", (unsigned)r->area);   /* fix021 */
    return n > 0 && (size_t)n < size ? n : 0;
}
/* the record's text (key=value lines, any line end, unknown keys ignored); 1 = a valid record */
static inline int cs1_rec_parse(const char *t, cs1_rec *r)
{
    cs1_rec o = {CS1_REC_NONE, -1, 0};
    while (t && *t) {
        const char *e = t;
        while (*e && *e != '\n' && *e != '\r') e++;
        size_t n = (size_t)(e - t);
        if (n == 8 && !strncmp(t, "mode=net", 8)) o.mode = CS1_REC_NET;
        else if (n == 10 && !strncmp(t, "mode=split", 10)) o.mode = CS1_REC_SPLIT;
        else if (n == 6 && !strncmp(t, "role=", 5) && (t[5] == '0' || t[5] == '1')) o.role = t[5] - '0';
        else if (n == 13 && !strncmp(t, "area=", 5)) {   /* fix021: the section of the write */
            uint32_t a = 0;
            int ok = 1;
            for (int i = 5; i < 13; i++) {
                char c = t[i];
                unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 99u;
                if (d > 15) ok = 0;
                a = a << 4 | (d & 15u);
            }
            if (ok) o.area = a;
        }
        t = e;
        while (*t == '\n' || *t == '\r') t++;
    }
    if (o.mode != CS1_REC_NET) o.role = -1;
    if (r) *r = o;
    return o.mode == CS1_REC_SPLIT || (o.mode == CS1_REC_NET && o.role >= 0);
}
/* what to record with a co-op write: a network game with this PC's known role, else the split screen, else the last record of this co-op stay (a session
 * that ended inside its level, Episode 2 after the end of Episode 1: the game goes on as the character it played) */
static inline cs1_rec cs1_rec_now(int net_started, int net_role, int split, const cs1_rec *last)
{
    cs1_rec r = {CS1_REC_NONE, -1, 0};
    if (net_started && (net_role == 0 || net_role == 1)) r.mode = CS1_REC_NET, r.role = net_role;
    else if (split) r.mode = CS1_REC_SPLIT;
    else if (last) r = *last;
    return r;
}

/* rule 6 at the host's title Continue: the role this host must play now (0 / 1), or -1 = keep the roles. coop_on: the co-op save is on; host / started: this
 * PC hosts a started network session; first: no level has been loaded in this session yet (the roles may still change); file: the co-op file exists;
 * rec: the record read next to it; cur: the host's role now */
enum { CS1_LK_KEEP = -1 };
enum { CS1_LKW_NONE = 0, CS1_LKW_NOT_COOP, CS1_LKW_NOT_HOST, CS1_LKW_PLAYED, CS1_LKW_NO_FILE, CS1_LKW_NO_REC, CS1_LKW_SPLIT, CS1_LKW_SAME, CS1_LKW_SWAP };
static inline int cs1_lock_role(int coop_on, int host, int started, int first, int file, const cs1_rec *rec, int cur, int *why)
{
    int w = CS1_LKW_SWAP, r = CS1_LK_KEEP;
    if (!coop_on) w = CS1_LKW_NOT_COOP;
    else if (!host || !started) w = CS1_LKW_NOT_HOST;
    else if (!first) w = CS1_LKW_PLAYED;
    else if (!file) w = CS1_LKW_NO_FILE;
    else if (!rec || rec->mode == CS1_REC_NONE || (rec->mode == CS1_REC_NET && rec->role < 0)) w = CS1_LKW_NO_REC;
    else if (rec->mode == CS1_REC_SPLIT) w = CS1_LKW_SPLIT;
    else if (rec->role == cur) w = CS1_LKW_SAME;
    else r = rec->role;
    if (why) *why = w;
    return r;
}
static inline const char *cs1_lkw_name(int w)
{
    switch (w) {
    case CS1_LKW_NOT_COOP: return "the co-op save is off";
    case CS1_LKW_NOT_HOST: return "not the host of a started network session";
    case CS1_LKW_PLAYED: return "a level of this session was loaded already (the roles stay)";
    case CS1_LKW_NO_FILE: return "no co-op save file";
    case CS1_LKW_NO_REC: return "the co-op save has no role record (made before round 16)";
    case CS1_LKW_SPLIT: return "the co-op save was made in split screen (both characters on one PC)";
    case CS1_LKW_SAME: return "the roles are already the save's";
    case CS1_LKW_SWAP: return "the roles change to the save's";
    default: return "-";
    }
}

/* rule 5: the split screen counts as a co-op game now (armed = menu.c applied the split; menu = the title menu page is up; level = a level runs) */
static inline int cs1_split_now(int armed, int menu, int level) { return armed && (menu || level); }

typedef struct {
    int active;               /* rule 1 */
    int in_coop_level;        /* a level that ran while co-op was on is still loaded */
    unsigned ons, offs;       /* transitions */
    unsigned redirected, passed, hidden;   /* storage calls on the co-op file / left on the solo file (co-op off); slot lookups answered "empty" */
    unsigned seeded;          /* co-op reads served from the solo file (read only) before the co-op file exists */
    int forced;               /* LAB: -1 = auto, 0 = off, 1 = on */
} cs1_t;

static inline void cs1_init(cs1_t *s) { memset(s, 0, sizeof *s), s->forced = -1; }

/* the solo file name the game uses, any case */
static inline int cs1_is_solo(const char *n)
{
    const char *w = CS1_SOLO_NAME;
    if (!n) return 0;
    for (; *w; w++, n++) {
        char c = *n;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != *w) return 0;
    }
    return *n == 0;
}

/* rule 1, once per frame: session = the network session has started (roles final), level = this PC has a level loaded (logged only), at_menu = the main title
 * menu is up (its manager updates). Returns the new state. split (rule 5, cs1_step2): the split screen counts now (cs1_split_now). */
static inline int cs1_step2(cs1_t *s, int session, int split, int level, int at_menu)
{
    int on;
    if (s->forced >= 0) on = s->forced;
    else {
        s->in_coop_level = s->active && level;
        on = session || split || (s->active && !at_menu);
    }
    if (on && !s->active) s->ons++;
    if (!on && s->active) s->offs++;
    s->active = on;
    return on;
}
static inline int cs1_step(cs1_t *s, int session, int level, int at_menu) { return cs1_step2(s, session, 0, level, at_menu); }

/* rule 2: where this storage call goes. is_write: FileWrite / FileDelete; coop_exists: the co-op file is there now */
enum { CS1_STOCK = 0, CS1_COOP = 1, CS1_SEED = 2 };
static inline int cs1_route(cs1_t *s, const char *name, int is_write, int coop_exists)
{
    if (!cs1_is_solo(name)) return CS1_STOCK;
    if (!s->active) {
        s->passed++;
        return CS1_STOCK;
    }
    if (is_write || coop_exists) {
        s->redirected++;
        return CS1_COOP;
    }
    s->seeded++;
    return CS1_SEED;   /* the solo file, read only */
}

/* rule 3: the slot entries read as empty now */
static inline int cs1_slots_hidden(cs1_t *s, int coop_file_exists)
{
    if (s->active && !coop_file_exists) {
        s->hidden++;
        return 1;
    }
    return 0;
}

#endif
