/*
 * test_bag1.c - rev1 live round 14, task 3 (native/bag1.h): each puppet carries and holds its owner's real guns, so both PCs show the same weapons.
 * The m14a bags (LIVE, 1-1 after a New Game): host Jill M92F (current) + knife; the host's puppet of Parker: Government (the level's partner set) + knife;
 * guest Parker 10B0 (current) 1070 10A0; the guest's puppet of Jill: unarmed only. Before the fix the two PCs show different guns in both humans' hands;
 * after the plan's steps (one per pass) each puppet holds its owner's current gun. Knives, grenades, the scanner are never added.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/bag1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

enum { M92F = 0x80051000, GOV = 0x80051010, H10B0 = 0x800510b0, H1070 = 0x80051070, H10A0 = 0x800510a0, KNFJ = 0x80051800, KNFC = 0x80051820, UNARMED = 0x80051b80,
       SCANNER = 0x80051b30, GND = 0x80051900, MP5 = 0x80051200, M3 = 0x80051310 };

typedef struct { uint32_t w[BG_SLOTS]; int cur; } bag;

static uint32_t held(const bag *b) { return b->cur >= 0 && b->cur < BG_SLOTS ? b->w[b->cur] : 0; }
static bg_msg owner_msg(const bag *b)
{
    bg_msg m;
    memset(&m, 0, sizeof m);
    for (int k = 0; k < 3; k++) m.cls[k] = b->w[k];
    m.cur = (uint8_t)b->cur, m.cur_cls = held(b);
    return m;
}
/* the stock AddWeapon puts a gun in the first free weapon slot (FUN_005C7A70) */
static int add(bag *b, uint32_t c)
{
    for (int i = 0; i < 3; i++)
        if (!b->w[i]) { b->w[i] = c; return 1; }
    return 0;
}
/* the stock per-item removal (cBioItemPack_vf7): the slot cleared, the current index reset when it was held */
static void rem(bag *b, int i)
{
    b->w[i] = 0;
    if (b->cur == i) b->cur = 0xF;
}
static int follow(const bag *owner, bag *puppet, int fixed)
{
    bg_msg m = owner_msg(owner);
    uint8_t buf[BG_LEN];
    bg_msg d;
    bg_enc(buf, &m);
    if (!bg_dec(buf, BG_LEN, &d)) return -1;
    int steps = 0;
    for (int pass = 0; pass < 10 && fixed; pass++) {
        bg_step s = bg_plan(&d, puppet->w, puppet->cur, !puppet->w[0] || !puppet->w[1] || !puppet->w[2]);
        if (s.act == BG_NONE) break;
        if (s.act == BG_ADD) add(puppet, s.cls);
        else if (s.act == BG_EQUIP) puppet->cur = s.index;
        else if (s.act == BG_REMOVE) {
            if (fixed < 2) break;   /* fixed 1 = the round-14 glue (no removal): a full bag stays */
            if (s.index == puppet->cur || s.index < 0 || s.index > 2) return -2;   /* never the held gun, only a weapon slot */
            rem(puppet, s.index);
        }
        steps++;
    }
    return steps;
}

static void t_m14a(void)
{
    for (int fixed = 0; fixed <= 1; fixed++) {
        bag hjill = {{M92F, 0, 0, 0, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED, 0, 0, 0}, 0};
        bag hpup_parker = {{GOV, 0, 0, 0, 0, 0, 0, 0, 0, 0, KNFC, UNARMED, 0, 0, 0}, 0};
        bag gparker = {{H10B0, H1070, H10A0, 0, 0, 0, 0, 0, 0, 0, 0, UNARMED, 0, 0, 0}, 0};
        bag gpup_jill = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, UNARMED, 0, 0, 0}, 11};
        follow(&gparker, &hpup_parker, fixed);   /* on the host */
        follow(&hjill, &gpup_jill, fixed);       /* on the guest */
        int same_parker = held(&hpup_parker) == held(&gparker), same_jill = held(&gpup_jill) == held(&hjill);
        if (!fixed) {
            CHECK(!same_parker && !same_jill, "B before the fix: Parker holds a Government on the host but a 10B0 on the guest; Jill an M92F on the host but nothing on the guest");
            printf("B before the fix: host shows Parker %08x (real %08x), guest shows Jill %08x (real %08x)\n", held(&hpup_parker), held(&gparker), held(&gpup_jill), held(&hjill));
        } else {
            CHECK(same_parker && same_jill, "B the fix: each puppet holds its owner's current gun on the other PC");
            CHECK(hpup_parker.w[0] == GOV && hpup_parker.w[1] == H10B0 && hpup_parker.w[2] == H1070, "B the owner's current gun is added first, then the others while a weapon slot is free");
            CHECK(hpup_parker.w[10] == KNFC && gpup_jill.w[11] == UNARMED, "B nothing is taken away (the puppet's knife / unarmed stay)");
        }
    }
}

static void t_rules(void)
{
    CHECK(bg_is_gun(M92F) && bg_is_gun(MP5) && bg_is_gun(M3) && bg_is_gun(0x80051400) && bg_is_gun(0x80051100), "C handguns, machine guns, shotguns, rifles, magnums are guns");
    CHECK(!bg_is_gun(KNFJ) && !bg_is_gun(GND) && !bg_is_gun(SCANNER) && !bg_is_gun(UNARMED) && !bg_is_gun(0) && !bg_is_gun(0x80031000), "C knives, grenades, the scanner, unarmed, enemies are not");
    bg_msg m;
    memset(&m, 0, sizeof m);
    m.cur_cls = KNFJ;   /* the owner holds his knife: nothing to do */
    uint32_t p[BG_SLOTS] = {GOV};
    CHECK(bg_plan(&m, p, 0, 1).act == BG_NONE, "C an owner with his knife in hand: no add, no equip");
    m.cur_cls = H10B0;
    uint32_t full[BG_SLOTS] = {GOV, M92F, MP5};
    bg_step r = bg_plan(&m, full, 0, 0);
    CHECK(r.act == BG_REMOVE && r.index == 1 && r.cls == M92F, "C rev1 live 15: a full bag makes room: the first gun that is not his and not held (never index 0 = held)");
    uint32_t fullown[BG_SLOTS] = {H10B0, H1070, H10A0};
    m.cls[0] = H10B0, m.cls[1] = H1070, m.cls[2] = H10A0;
    CHECK(bg_plan(&m, fullown, 1, 0).act == BG_EQUIP, "C his guns already there: only the equip, nothing taken away");
    CHECK(bg_plan(&m, fullown, 0, 0).act == BG_NONE, "C his guns, his current held: nothing");
    uint32_t heldonly[BG_SLOTS] = {MP5, 0, 0, GND, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED};
    m.cur_cls = 0, memset(m.cls, 0, sizeof m.cls);
    CHECK(bg_plan(&m, heldonly, 0, 1).act == BG_NONE, "C an owner without a gun: nothing taken away");
    m.cur_cls = H10B0, m.cls[0] = H10B0;
    uint32_t onlyheld[BG_SLOTS] = {MP5, M3, GOV, GND, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED};
    bg_step r2 = bg_plan(&m, onlyheld, 0, 0);
    CHECK(r2.act == BG_REMOVE && r2.index == 1, "C the held MP5 stays, the M3 goes");
    CHECK(bg_spare(&m, onlyheld, 0) != 3 && bg_spare(&m, onlyheld, 0) != 10, "C a grenade / knife is never a spare");
    m.cls[0] = 0;
    uint32_t has[BG_SLOTS] = {GOV, H10B0};
    bg_step s = bg_plan(&m, has, 0, 1);
    CHECK(s.act == BG_EQUIP && s.index == 1, "C the puppet carries it already: only the equip");
    CHECK(bg_plan(&m, has, 1, 1).act == BG_NONE, "C held already: nothing");
    uint8_t b[BG_LEN];
    bg_msg d;
    bg_enc(b, &m);
    CHECK(bg_dec(b, BG_LEN, &d) && bg_same(&d, &m), "C wire round trip");
    b[0] = 2;
    CHECK(!bg_dec(b, BG_LEN, &d), "C an unknown version is refused");
}

/* rev1 live 15 (task 3): the 1-3 cut start (m14g / m14f LIVE): the host's puppet of the guest's Jill gets the HOST profile's full set MP5 / M3 / Remington /
 * grenade (MP5 held), the guest's own Jill carries 10B0 (held) / 1070 / 10A0 (DLC guns). Before (round 14) a full bag got nothing: the host showed Jill with
 * an MP5. Now: the spare guns go one by one, his guns come, his current is held. */
enum { REM = 0x80051400 };
static void t_cut13(void)
{
    for (int fixed = 1; fixed <= 2; fixed++) {
        bag gjill = {{H10B0, H1070, H10A0, 0, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED, SCANNER, 0, 0}, 0};
        bag hpup = {{MP5, M3, REM, GND, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED, SCANNER, 0, 0}, 0};
        int st = follow(&gjill, &hpup, fixed);
        int same = held(&hpup) == held(&gjill);
        int all = bg_find(hpup.w, H10B0) >= 0 && bg_find(hpup.w, H1070) >= 0 && bg_find(hpup.w, H10A0) >= 0;
        if (fixed == 1) {
            CHECK(!same && held(&hpup) == MP5, "E before the fix: the host's puppet of Jill holds the host profile's MP5, the guest's Jill a 10B0");
            printf("E before the fix: the host shows Jill with %08x (her real gun %08x), %d steps\n", held(&hpup), held(&gjill), st);
        } else {
            CHECK(st > 0 && same && all, "E the fix: the puppet holds her 10B0 and carries her three guns");
            CHECK(hpup.w[3] == GND && hpup.w[10] == KNFJ && hpup.w[11] == UNARMED && hpup.w[12] == SCANNER, "E the grenade, knife, unarmed, scanner stay");
            printf("E the fix: the host shows Jill with %08x, her bag %08x %08x %08x (%d steps)\n", held(&hpup), hpup.w[0], hpup.w[1], hpup.w[2], st);
        }
    }
    /* the archive path of FUN_008A29A0 */
    char p[96];
    CHECK(bg_arc_path(H10B0, p, sizeof p) && !strcmp(p, "dlc\\archive\\game\\chara\\wp\\wp10b0\\wp10b0"), "F 10B0: the DLC path (dat\\DAT_02\\dlc\\...)");
    CHECK(bg_arc_path(H1070, p, sizeof p) && !strcmp(p, "dlc\\archive\\game\\chara\\wp\\wp1070\\wp1070"), "F 1070: the DLC path too (the nativePC wp1070.arc is another file)");
    CHECK(bg_arc_path(H10A0, p, sizeof p) && !strcmp(p, "dlc\\archive\\game\\chara\\wp\\wp10a0\\wp10a0"), "F 10A0: the DLC path");
    CHECK(bg_arc_path(M92F, p, sizeof p) && !strcmp(p, "archive\\game\\chara\\wp\\wp1000\\wp1000"), "F M92F: the plain path");
    CHECK(bg_arc_path(GOV, p, 10) == 0, "F a short buffer is refused");
    CHECK(bg_is_dlc(H10B0) && bg_is_dlc(H1070) && bg_is_dlc(H10A0) && !bg_is_dlc(M92F) && !bg_is_dlc(GOV), "F the DLC list of FUN_007EAD00");
    /* the image (when it is there): FUN_007EAD00 compares exactly these three gun classes and FUN_009B12C0 starts with the bytes the glue checks */
    FILE *im = fopen("../../image/rerev-image.bin", "rb");
    if (im) {
        unsigned char c[0x60], ex[14];
        int ok = fseek(im, 0x7EAD00L - 0x400000L, SEEK_SET) == 0 && fread(c, 1, sizeof c, im) == sizeof c && fseek(im, 0x9B12C0L - 0x400000L, SEEK_SET) == 0 && fread(ex, 1, 14, im) == 14;
        fclose(im);
        static const unsigned char k1070[5] = {0x3D, 0x70, 0x10, 0x05, 0x80}, k10a0[5] = {0x3D, 0xA0, 0x10, 0x05, 0x80}, k10b0[5] = {0x3D, 0xB0, 0x10, 0x05, 0x80};
        static const unsigned char exs[14] = {0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x9C, 0x24, 0x0C, 0x01, 0x00, 0x00};
        CHECK(ok && !memcmp(c + 0x13, k1070, 5) && !memcmp(c + 0x1A, k10a0, 5) && !memcmp(c + 0x21, k10b0, 5), "F the image: FUN_007EAD00's cmp eax, 0x80051070 / A0 / B0");
        CHECK(ok && !memcmp(ex, exs, 14), "F the image: FUN_009B12C0's first bytes");
    } else printf("(no image: the DLC list bytes are not checked)\n");
}

/* rev1 live 15 (m15a3 LIVE): with the DLC archives not loaded (bagdlc 0, the default after the host's fatal 'Failed open file ... em1000.mod' at the 1-1 ->
 * 1-2 change), the DLC guns must leave the owner's list BEFORE the plan: else a full puppet bag gives up a gun for a DLC gun it cannot get */
static void t_dlcfilter(void)
{
    bag gjill = {{H10B0, H1070, H10A0, 0, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED, 0, 0, 0}, 0};
    for (int filt = 0; filt <= 1; filt++) {
        bag hpup = {{MP5, M3, REM, GND, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED, 0, 0, 0}, 0};   /* the 1-3 cut-start set */
        bg_msg m = owner_msg(&gjill);
        if (filt) {
            for (int k = 0; k < 3; k++)
                if (bg_is_dlc(m.cls[k])) m.cls[k] = 0;
            if (bg_is_dlc(m.cur_cls)) m.cur_cls = 0;
        }
        for (int pass = 0; pass < 10; pass++) {   /* the glue: an ADD of a DLC gun fails (no archive), a REMOVE works */
            bg_step s = bg_plan(&m, hpup.w, hpup.cur, !hpup.w[0] || !hpup.w[1] || !hpup.w[2]);
            if (s.act == BG_NONE) break;
            if (s.act == BG_ADD && !bg_is_dlc(s.cls)) add(&hpup, s.cls);
            else if (s.act == BG_EQUIP) hpup.cur = s.index;
            else if (s.act == BG_REMOVE) rem(&hpup, s.index);
        }
        if (!filt) CHECK(hpup.w[1] == 0, "G unfiltered (the model before the filter): the puppet's M3 is taken away for a DLC gun it cannot get");
        else CHECK(hpup.w[0] == MP5 && hpup.w[1] == M3 && hpup.w[2] == REM && held(&hpup) == MP5, "G filtered: the puppet's bag is untouched (no gun taken away for a gun it cannot get)");
    }
}

static char *slurp(const char *p)
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
    return s;
}
static void t_sources(void)
{
    char *g = slurp("../native/bag1_game.inc"), *w = slurp("../native/world1_game.c"), *s = slurp("../native/story1_game.inc"), *p = slurp("../native/coop_proto.h");
    char *gv = slurp("../native/give1.c");
    CHECK(g && strstr(g, "bg_fsm(BG_ADDWEAPON, pup, s.cls);   /* the stock AddWeapon loads the archive itself (FUN_005CE180) */"), "D fix021: the stock AddWeapon loads the archive (no own load before it)");
    CHECK(g && strstr(g, "BG_DTI_ARCHIVE 0x11E0274u"), "D the stock archive DTI (for the existence check)");
    CHECK(g && strstr(g, "if (!bg_fsm_ok(BG_ADDWEAPON))") && strstr(g, "if (!bg_fsm_ok(BG_EQUIPSLOT))"), "D the FSM actions' bytes are checked first");
    {   /* m14e (LIVE): the stock load of a missing archive is fatal: the stock path + the stock existence check (FUN_009B12C0), never a load by the mod */
        const char *pa = g ? strstr(g, "if (!bg_arc_path(cls, path, sizeof path)) return 0;") : NULL, *ex = g ? strstr(g, "if (!bg_arc_exists(cls, path)) {") : NULL;
        CHECK(pa && ex && pa < ex && strstr(g, "#define BG_RES_EXISTS 0x9B12C0u") && !strstr(g, "GetFileAttributesA"), "D the stock path, then the stock existence check");
        CHECK(g && strstr(g, "bg_exists_sig[] = {0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x9C, 0x24, 0x0C, 0x01, 0x00, 0x00};"), "D FUN_009B12C0's bytes in the glue");
        CHECK(g && !strstr(g, "((bg_load_fn)") && !strstr(g, "bg_arch[bg_narch]"), "D r48 TRAP (fix021): the bag mirror holds NO archive reference of its own (no sResource +0x30 load)");
        CHECK(gv && !strstr(gv, "((load_fn)(uintptr_t)fn)"), "D r48 TRAP (fix021): give1 holds NO archive reference of its own (the stock grenade add loads it)");
        CHECK(g && strstr(g, "static volatile int f_bagdlc = 1, f_bagroom = 1;") && strstr(g, "bg_step s = bg_plan2(&want, pc, pcur, free_slot);") && strstr(g, "bg_mirrorable(&want);"),
              "D fix021: DLC guns and the room on by default through the stock path; the plan sees what this PC can show");
        CHECK(g && strstr(g, "bg_fsm_slot(BG_ADDWEAPON, pup, s.cls, (uint32_t)s.index);") && strstr(g, "s.index == pcur") && strstr(g, "!bg_item_room(s.cls)"),
              "D fix021: the room by the stock replace (AddWeapon into that slot), never the held index, only with room in the item table");
    }
    CHECK(g && strstr(g, "vp0 != m0 && vp0 != m1"), "D nothing while a cutscene camera is on viewport 0");
    CHECK(w && strstr(w, "#include \"bag1_game.inc\"") && strstr(w, "bag_pass(now);"), "D in the frame");
    CHECK(s && strstr(s, "sub == CP_W1_BAG") && strstr(s, "bag_rx(b, len)"), "D op_ext routes CP_W1_BAG");
    CHECK(p && strstr(p, "#define CP_W1_BAG 26"), "D CP_W1_BAG = 26 (inside CP_W1_EXT_FIRST..LAST)");
    free(g), free(w), free(s), free(p), free(gv);
}

/* fix021 (0.2.1): the r48 rules - no own archive, the stock existence check with a stock fallback, the 16-slot item table, the stock replace */
static void t_r48(void)
{
    CHECK(bg_fallback(H10B0, 1) == H10B0 && bg_fallback(M92F, 1) == M92F, "E a gun whose files are here stays itself");
    CHECK(bg_fallback(H10B0, 0) == M92F && bg_fallback(H1070, 0) == M92F && bg_fallback(H10A0, 0) == M92F, "E a DLC handgun without its files: the stock M92F");
    CHECK(bg_fallback(MP5, 0) == 0 && bg_fallback(M92F, 0) == 0, "E no files and no stock sibling: nothing (never a missing archive to the stock loader: the fatal 'Failed open file')");
    uint32_t tab[16];
    for (int i = 0; i < 16; i++) tab[i] = 0x80052000u + 0x10u * (uint32_t)i;
    CHECK(!bg_item_table_ok(tab, H10B0) && bg_item_table_ok(tab, 0x80052010u), "E the item table full: a new class is refused, a listed class is fine");
    tab[7] = BG_ITEM_EMPTY;
    CHECK(bg_item_table_ok(tab, H10B0), "E a free item slot: the new class may come");
    bg_msg m;
    memset(&m, 0, sizeof m);
    m.cur_cls = H10B0, m.cls[0] = H10B0;
    uint32_t full[BG_SLOTS] = {GOV, M92F, MP5};
    bg_step r = bg_plan2(&m, full, 0, 0);
    CHECK(r.act == BG_REPLACE && r.index == 1 && r.cls == H10B0, "E a full bag: the stock replace of slot 1 (not held) by his current gun");
    r = bg_plan2(&m, full, 1, 0);
    CHECK(r.act == BG_REPLACE && r.index == 0 && r.cls == H10B0, "E the held slot 1 is never replaced");
    uint32_t heldall[BG_SLOTS] = {GOV, 0, 0};
    CHECK(bg_plan2(&m, heldall, 0, 1).act == BG_ADD, "E a free slot: a plain add");
    uint32_t onlyheld[BG_SLOTS] = {GOV, M92F, MP5, GND, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED, 0, 0, SCANNER};
    m.cls[0] = GOV, m.cls[1] = M92F, m.cls[2] = MP5;   /* all three are his: nothing may go */
    m.cur_cls = H10B0;
    r = bg_plan2(&m, onlyheld, 0, 0);
    CHECK(r.act == BG_NONE, "E all three guns are his too: nothing replaced (never the knife, grenade, scanner, unarmed)");
    /* r48 model: three level instances (two game overs, then the section change) with the partner's three DLC pistols: references the MOD holds */
    int mod_refs_020 = 0, mod_refs_021 = 0;
    for (int level = 0; level < 3; level++) {
        if (level == 0) mod_refs_020 += 3;   /* r48: bg_load_archive kept one reference per DLC class for the whole session */
        mod_refs_021 += 0;                   /* 0.2.1: the stock add's own reference, released at the item's removal / the level end */
    }
    CHECK(mod_refs_020 == 3 && mod_refs_021 == 0, "E r48 TRAP: the 0.2.0 path held 3 archive references over all level instances; 0.2.1 holds none");
}

/* fix021 (0.2.1): the host's seed of an unarmed guest (variant (i)) and the puppet holding the same non-gun as its owner */
typedef struct { uint32_t own[BG_SLOTS]; int seed_have, applied, armed_seen, adds; } gst_t;
static void g_level(gst_t *g, const uint32_t own0[3]) { memset(g, 0, sizeof *g); for (int i = 0; i < 3; i++) g->own[i] = own0[i]; g->own[11] = UNARMED; }
static void g_pass(gst_t *g, const bg_msg *seed, int settled)
{
    if (bg_has_gun(g->own)) g->armed_seen = 1;
    if (!bg_seed_wanted(1, g->seed_have, g->applied, bg_has_gun(g->own), g->armed_seen, settled)) return;
    g->applied = 1;
    for (int k = 0; k < 3; k++) {
        uint32_t c = seed->cls[k];
        if (!bg_is_gun(c) || bg_find(g->own, c) >= 0) continue;
        for (int i = 0; i < 3; i++) if (!g->own[i]) { g->own[i] = c, g->adds++; break; }
    }
}
static void t_seed(void)
{
    bg_msg seed;
    memset(&seed, 0, sizeof seed);
    seed.cls[0] = 0x80051060u, seed.cls[1] = GND, seed.cls[2] = 0;   /* the host's save: Jessica's PC356 and a grenade (not a gun) */
    uint8_t b[BG_LEN];
    bg_msg back;
    bg_enc_seed(b, &seed);
    CHECK(b[0] == 2 && bg_dec_seed(b, BG_LEN, &back) && back.cls[0] == 0x80051060u && !bg_dec(b, BG_LEN, &back), "F the seed is version 2 of CP_W1_BAG (a version-1 reader ignores it)");
    gst_t g;
    const uint32_t none[3] = {0, 0, 0}, own[3] = {GOV, 0, 0};
    g_level(&g, none), g.seed_have = 1;
    g_pass(&g, &seed, 0);
    CHECK(!g.applied, "F not before the stock start has settled (3 s)");
    g_pass(&g, &seed, 1);
    CHECK(g.applied && g.own[0] == 0x80051060u && g.adds == 1 && g.own[1] == 0, "F an empty guest after a section-change save: the host's guns, guns only (no grenade)");
    g_pass(&g, &seed, 1);
    CHECK(g.adds == 1, "F a repeat: never twice");
    g_level(&g, own), g.seed_have = 1;
    g_pass(&g, &seed, 1);
    CHECK(!g.applied && g.own[0] == GOV && g.adds == 0, "F a guest with its own gun: its guns stay, nothing added");
    g_level(&g, none), g.seed_have = 1;
    g.own[0] = M92F, g_pass(&g, &seed, 0), g.own[0] = 0;   /* picked up / given in the first seconds, then dropped / given away */
    g_pass(&g, &seed, 1);
    CHECK(!g.applied && g.adds == 0, "F a gun seen in this level (picked up / given / dealt): the seed never applies");
    g_level(&g, none);   /* the roles swapped (rule 6) or the 9-2 -> 10-1 change: a NEW level instance, the seed of the character played now */
    bg_msg s2 = seed;
    s2.cls[0] = M92F;
    g.seed_have = 1, g_pass(&g, &s2, 1);
    CHECK(g.applied && g.own[0] == M92F, "F a new level instance (roles swapped / 9-2 -> 10-1): one application of that level's seed");
    CHECK(!bg_seed_wanted(0, 1, 0, 0, 0, 1), "F the host never applies a seed");
    /* the partner holds no gun: the puppet holds the same */
    bg_msg m;
    memset(&m, 0, sizeof m);
    m.cur_cls = UNARMED;
    uint32_t p[BG_SLOTS] = {GOV, 0, 0, 0, 0, 0, 0, 0, 0, 0, KNFJ, UNARMED};
    bg_step r = bg_plan2(&m, p, 0, 1);
    CHECK(r.act == BG_EQUIP && r.index == 11, "F the partner holds nothing: the puppet puts its gun away (unarmed)");
    m.cur_cls = KNFJ;
    r = bg_plan2(&m, p, 11, 1);
    CHECK(r.act == BG_EQUIP && r.index == 10, "F the partner holds his knife: the puppet holds its knife");
    m.cur_cls = SCANNER;
    CHECK(bg_plan2(&m, p, 11, 1).act == BG_NONE, "F a non-gun the puppet does not carry: nothing");
    {   /* dw1 live: the guest held 1070 (Government, 10B0, 1070), the stock add put M92F into the held slot: the host's puppet (Government, 10B0, 1070 held) */
        bg_msg o;
        memset(&o, 0, sizeof o);
        o.cls[0] = GOV, o.cls[1] = H10B0, o.cls[2] = M92F, o.cur_cls = M92F;
        uint32_t pup[BG_SLOTS] = {GOV, H10B0, H1070};
        bg_step a = bg_plan2(&o, pup, 2, 0);
        CHECK(a.act == BG_EQUIP && a.index != 2, "G dw1: the only spare is held: another gun in hand first (never the held one replaced)");
        bg_step b2 = bg_plan2(&o, pup, a.index, 0);
        CHECK(b2.act == BG_REPLACE && b2.index == 2 && b2.cls == M92F, "G dw1: then the old gun's slot is replaced by his M92F");
        pup[2] = M92F;
        bg_step c2 = bg_plan2(&o, pup, a.index, 0);
        CHECK(c2.act == BG_EQUIP && c2.index == 2, "G dw1: and his M92F goes into the hand");
    }
}

/* fix021 kit: the host's start kit (knife, reserve ammo, herbs) of a guest whose bag the stock start never filled (fr1 B0, flca 3-1) */
typedef struct { uint32_t own[BG_SLOTS]; int ammo[BK_AMMO], herb, have, applied, knife_seen, knives_added, sets; } gk_t;
static void k_level(gk_t *g, const uint32_t own[BG_SLOTS]) { memset(g, 0, sizeof *g); memcpy(g->own, own, sizeof g->own); }
static void k_pass(gk_t *g, const bg_kit *kit, int is_client, int settled)
{
    if (is_client && bg_is_knife(g->own[BK_KNIFE_ITEM])) g->knife_seen = 1;
    if (!bg_kit_wanted(is_client, g->have, g->applied, bg_is_knife(kit->knife), bg_is_knife(g->own[BK_KNIFE_ITEM]), g->knife_seen, settled)) return;
    g->applied = 1;
    bg_kit_plan p = bg_kit_plan_of(kit, g->own, g->ammo, g->herb);
    if (p.knife) g->own[BK_KNIFE_ITEM] = p.knife, g->knives_added++;
    for (int i = 0; i < BK_AMMO; i++) if (p.ammo[i] >= 0) g->ammo[i] = p.ammo[i], g->sets++;
    if (p.herb >= 0) g->herb = p.herb, g->sets++;
}
static void t_kit(void)
{
    bg_kit kit, back;
    memset(&kit, 0, sizeof kit);
    kit.knife = KNFC, kit.ammo[0] = 30, kit.herb = 1;   /* fr1 B0: the host's puppet of Parker (the 1-1 partner set): Government + KnfChris + 30 rounds + 1 herb */
    uint8_t b[BK_LEN];
    bg_msg m;
    CHECK(bg_enc_kit(b, &kit) == BK_LEN && BK_LEN <= 92 && b[0] == 3, "K the kit is version 3 of CP_W1_BAG, 31 bytes (W1_EXT_MAX 92)");
    CHECK(bg_dec_kit(b, BK_LEN, &back) && back.knife == KNFC && back.ammo[0] == 30 && back.herb == 1 && back.ammo[1] == 0, "K the kit decodes");
    CHECK(!bg_dec(b, BK_LEN, &m) && !bg_dec_seed(b, BK_LEN, &m), "K a version-1 / version-2 reader ignores the kit");
    uint8_t s[BG_LEN];
    bg_msg sm;
    memset(&sm, 0, sizeof sm);
    bg_enc_seed(s, &sm);
    CHECK(!bg_dec_kit(s, BG_LEN, &back), "K the kit reader ignores the seed");
    CHECK(bg_is_knife(KNFJ) && bg_is_knife(KNFC) && !bg_is_knife(GOV) && !bg_is_knife(UNARMED) && !bg_is_knife(GND), "K the knives are the 0x18 family");

    const uint32_t gparker[BG_SLOTS] = {H10B0, H1070, H10A0, 0, 0, 0, 0, 0, 0, 0, 0, UNARMED, 0, 0, 0};   /* fr1 B0: the guest's own Parker */
    gk_t g;
    k_level(&g, gparker), g.have = 1;
    k_pass(&g, &kit, 1, 0);
    CHECK(!g.applied, "K not before the stock start has settled (3 s)");
    k_pass(&g, &kit, 1, 1);
    CHECK(g.applied && g.own[BK_KNIFE_ITEM] == KNFC && g.ammo[0] == 30 && g.herb == 1 && g.sets == 2 && g.knives_added == 1, "K fr1 B0: the guest's Parker gets the knife, 30 rounds, 1 herb");
    CHECK(g.own[0] == H10B0 && g.own[1] == H1070 && g.own[2] == H10A0, "K its guns stay as they are");
    k_pass(&g, &kit, 1, 1);
    CHECK(g.knives_added == 1 && g.sets == 2, "K a repeat: never twice");

    const uint32_t gjess[BG_SLOTS] = {0x80051060u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, UNARMED, 0, 0, 0};   /* flca 3-1: the guest's Jessica after the gun seed */
    bg_kit kj;
    memset(&kj, 0, sizeof kj);
    kj.knife = KNFC, kj.ammo[0] = 9, kj.ammo[2] = 7;
    k_level(&g, gjess), g.have = 1;
    k_pass(&g, &kj, 1, 1);
    CHECK(g.own[BK_KNIFE_ITEM] == KNFC && g.ammo[0] == 9 && g.ammo[2] == 7 && g.ammo[1] == 0 && g.sets == 2 && g.herb == 0, "K flca 3-1: Jessica's knife and her 9 / 7 rounds");

    k_level(&g, gparker), g.have = 1, g.ammo[0] = 12;   /* rounds picked up in the first seconds */
    k_pass(&g, &kit, 1, 1);
    CHECK(g.ammo[0] == 12 && g.herb == 1 && g.own[BK_KNIFE_ITEM] == KNFC, "K a count the guest has is kept (never lowered, nothing on top)");

    const uint32_t gfull[BG_SLOTS] = {GOV, 0, 0, 0, 0, 0, 0, 0, 0, 0, KNFC, UNARMED, 0, 0, 0};
    k_level(&g, gfull), g.have = 1;
    k_pass(&g, &kit, 1, 1);
    CHECK(!g.applied && g.ammo[0] == 0 && g.herb == 0, "K a guest with its knife (a filled bag, a checkpoint after the kit): nothing, no counts either");

    k_level(&g, gfull), g.have = 1;
    k_pass(&g, &kit, 1, 0);
    g.own[BK_KNIFE_ITEM] = 0;
    k_pass(&g, &kit, 1, 1);
    CHECK(!g.applied, "K a knife seen in this level instance: the kit never applies");

    bg_kit nok = kit;
    nok.knife = 0;
    k_level(&g, gparker), g.have = 1;
    k_pass(&g, &nok, 1, 1);
    CHECK(!g.applied && g.ammo[0] == 0 && g.herb == 0, "K the stock start set has no knife for this character (Hunk, a knifeless part): nothing at all");

    k_level(&g, gparker), g.have = 1;
    k_pass(&g, &kit, 0, 1);
    CHECK(!g.applied, "K the host never applies a kit");
    k_level(&g, gparker);
    k_pass(&g, &kit, 1, 1);
    CHECK(!g.applied, "K no kit received: nothing");

    k_level(&g, gparker), g.have = 1;   /* a new level instance: a reload to the section start brings back the unfilled bag: one application again */
    k_pass(&g, &kit, 1, 1);
    CHECK(g.applied && g.knives_added == 1, "K a new level instance (a reload to the section start): one application of that instance's kit");

    {   /* the glue: the stock AddWeapon with slot 10, the pack's vf11 checked by its bytes, only the host sends, the kit before the seed in bag_rx */
        char *w = slurp("../native/bag1_game.inc");
        CHECK(w && strstr(w, "bg_fsm_slot(BG_ADDWEAPON, me, p.knife, BK_KNIFE_ITEM)") && strstr(w, "ammo_ok = rd8(af + i) == bg_ammo_sig[i]")
                  && strstr(w, "if (!g_client && pup && !bg_kit_sent)") && strstr(w, "if (bg_dec_kit(b, len, &kt))")
                  && strstr(w, "bg_kit_sent = bg_kit_have = bg_kit_applied = bg_knife_seen = 0;"),
              "K the glue: the knife by the stock AddWeapon into item 10, vf11 checked, the host sends once per level instance, reset per level");
        free(w);
    }
}

/* fix021 (fw1 on de2598fc 23:04): no bag change before the stock level start has placed this PC's human; the settle counts from the placement */
static uint32_t gate_first_apply(int fixed, uint32_t placed_at, uint32_t end)
{   /* the barrier is open (gameplay camera) from t = 0; passes every 500 ms; returns the time of the first seed / kit application (0 = none) */
    uint32_t level_ms = 0;
    for (uint32_t t = 500; t <= end; t += 500) {
        int placed = t >= placed_at;
        if (fixed ? !bg_gate(1, placed) : 0) continue;
        if (!level_ms) level_ms = t;
        int settled = fixed ? bg_settled(level_ms, t) : (uint32_t)(t - level_ms) >= 3000u;
        if (bg_seed_wanted(1, 1, 0, 0, 0, settled)) return t;
    }
    return 0;
}
static void t_gate(void)
{
    uint32_t before = gate_first_apply(0, 13875, 30000), after = gate_first_apply(1, 13875, 30000);
    if (before < 13875) printf("  MODEL BEFORE THE FIX FAILS: fw1 23:04 (the barrier open by its timeout 13.9 s before the level start): the seed / kit at %u ms, the human placed at 13875 ms\n", before);
    CHECK(before && before < 13875, "G the model before the fix: the seed / kit before the placement (the fw1 23:04 crash timing)");
    CHECK(after >= 13875 + 3000 && after <= 13875 + 3500, "G fw1 23:04 timing: the seed / kit only 3 s after this PC's human is placed");
    CHECK(gate_first_apply(1, 0, 30000) == 3500 && gate_first_apply(0, 0, 30000) == 3500, "G the usual order (the barrier after LOAD_DONE, the human placed): the same 3 s as before");
    CHECK(gate_first_apply(1, 999999, 30000) == 0, "G a human never placed (a movie, a load): nothing");
    CHECK(!bg_settled(0, 5000) && bg_settled(1000, 4000) && !bg_settled(1000, 3999), "G the settle counts from the placement, 3 s");
    CHECK(!bg_gate(0, 1) && !bg_gate(1, 0) && bg_gate(1, 1), "G the gate: the gameplay camera AND the placed human");
    char *g = slurp("../native/bag1_game.inc");
    const char *gp = g ? strstr(g, "if (!bg_gate(1, bg_unit_placed(me))) return;") : NULL, *lm = g ? strstr(g, "if (!bg_level_ms) bg_level_ms = now ? now : 1;") : NULL;
    int nset = 0;
    for (const char *p = g; p && (p = strstr(p, "bg_settled(bg_level_ms, now)")); p++) nset++;
    CHECK(gp && lm && gp < lm && nset == 2 && !strstr(g, "(uint32_t)(now - bg_level_ms) >= 3000u"),
          "G the glue: bag_pass returns before the placement, the seed and the kit settle from it");
    const char *hk = g ? strstr(g, "static void hunk_bag_pass(") : NULL, *hkp = hk ? strstr(hk, "if (!bg_unit_placed(me)) return;") : NULL, *hka = hk ? strstr(hk, "bg_fsm(BG_ADDWEAPON, me, HK_START_GUN);") : NULL;
    CHECK(hkp && hka && hkp < hka, "G Hunk's start gun: not before the placement either");
    free(g);
}

/* fix021 (R1 f21r1 04:13): a partner report counts only in its own level instance; a mirror-added gun he no longer lists goes */
static void t_sync(void)
{
    /* the guest's 1-1 report (epoch 2) and the 1-2 puppet of Parker (the level's own Government) in the FIRST frame of epoch 3, before the reset */
    bg_msg old11;
    memset(&old11, 0, sizeof old11);
    old11.cls[0] = H10B0, old11.cls[1] = H1070, old11.cls[2] = H10A0, old11.cur_cls = H10B0;
    uint32_t pup12[BG_SLOTS] = {GOV, 0, 0, 0, 0, 0, 0, 0, 0, 0, KNFC, UNARMED};
    bg_step before = bg_plan2(&old11, pup12, 0, 1);   /* the 0.2.1 candidate's glue: the stale report planned at once */
    if (before.act == BG_ADD && before.cls == H10B0)
        printf("  MODEL BEFORE THE FIX FAILS: f21r1 04:13: the 1-1 report gives the 1-2 puppet %08x before the guest's fresh report (Government only)\n", (unsigned)before.cls);
    CHECK(before.act == BG_ADD && before.cls == H10B0, "S the model before the fix: the old level's report adds 10B0 to the new level's puppet");
    CHECK(!bg_peer_usable(1, 2, 3), "S the fix: a report of level instance 2 is not used in level instance 3");
    CHECK(bg_peer_usable(1, 3, 3) && !bg_peer_usable(0, 3, 3), "S a report of this level instance is used; none = nothing");
    /* the fresh 1-2 report: Government only */
    bg_msg fresh;
    memset(&fresh, 0, sizeof fresh);
    fresh.cls[0] = GOV, fresh.cur_cls = GOV;
    bg_step f = bg_plan2(&fresh, pup12, 0, 1);
    CHECK(f.act == BG_NONE, "S the fresh report on the untouched puppet: nothing to do (Government in hand)");
    uint32_t none[BG_ADDED_MAX] = {0};
    CHECK(bg_stale_mirror(&fresh, pup12, 0, none, 0) < 0, "S the puppet's own Government of the level start: never taken away");
    /* the f21r1 state: 10B0 already added by the stale report */
    uint32_t pupS2[BG_SLOTS] = {GOV, H10B0, 0, 0, 0, 0, 0, 0, 0, 0, KNFC, UNARMED};
    uint32_t added[BG_ADDED_MAX] = {H10B0};
    CHECK(bg_plan2(&fresh, pupS2, 0, 1).act == BG_NONE, "S the old plan: a free slot, so the stale 10B0 would stay for ever");
    CHECK(bg_stale_mirror(&fresh, pupS2, 0, added, 1) == 1, "S the fix: the mirror-added 10B0 he no longer lists goes (index 1)");
    CHECK(bg_stale_mirror(&fresh, pupS2, 1, added, 1) < 0, "S never the held gun");
    CHECK(bg_stale_mirror(&old11, pupS2, 0, added, 1) < 0, "S never a gun he still lists");
    uint32_t added_gov[BG_ADDED_MAX] = {GOV};
    bg_msg dlc;
    memset(&dlc, 0, sizeof dlc);
    dlc.cls[0] = H1070, dlc.cur_cls = H1070;
    uint32_t pupG[BG_SLOTS] = {GOV, H1070, 0, 0, 0, 0, 0, 0, 0, 0, KNFC, UNARMED};
    CHECK(bg_stale_mirror(&dlc, pupG, 1, none, 0) < 0, "S the puppet's own story gun he does not list (not added by the mirror): stays");
    CHECK(bg_stale_mirror(&dlc, pupG, 1, added_gov, 1) == 0, "S the same gun when the mirror had added it: goes");
    uint32_t pupK[BG_SLOTS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KNFC, UNARMED};
    uint32_t added_k[BG_ADDED_MAX] = {KNFC, UNARMED};
    CHECK(bg_stale_mirror(&fresh, pupK, -1, added_k, 2) < 0, "S never a knife / unarmed (only the three gun slots)");
    {
        char *g = slurp("../native/bag1_game.inc");
        CHECK(g && strstr(g, "if (!bg_peer_usable(bg_peer_have, bg_peer_epoch, w1_epoch(&N.w)) || !pup")
                  && strstr(g, "bg_peer = m, bg_peer_have = 1, bg_peer_epoch = w1_epoch(&N.w);")
                  && strstr(g, "bg_peer_have = bg_peer_have && bg_peer_epoch == w1_epoch(&N.w);")
                  && strstr(g, "if (ok) BGS.adds++, bg_note_added(s.cls);") && strstr(g, "if (ok) BGS.removes++, BGS.adds++, bg_note_added(s.cls);")
                  && strstr(g, "int k = bg_stale_mirror(&want, pc, pcur, bg_added, bg_nadded);"),
              "S the glue: the epoch of the report, the mirror's adds noted, the stale gun removed the stock way");
        free(g);
    }
}

int main(void)
{
    t_m14a();
    t_rules();
    t_cut13();
    t_dlcfilter();
    t_sources();
    t_r48();
    t_seed();
    t_kit();
    t_gate();
    t_sync();
    printf("test_bag1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
