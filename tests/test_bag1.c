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
    CHECK(g && strstr(g, "if (!bg_load_archive(s.cls)) return;") && strstr(g, "bg_fsm(BG_ADDWEAPON, pup, s.cls);"), "D the archive is loaded before the add (the l11a crash)");
    CHECK(g && strstr(g, "BG_DTI_ARCHIVE 0x11E0274u"), "D the stock archive DTI");
    CHECK(g && strstr(g, "if (!bg_fsm_ok(BG_ADDWEAPON))") && strstr(g, "if (!bg_fsm_ok(BG_EQUIPSLOT))"), "D the FSM actions' bytes are checked first");
    {   /* m14e (LIVE): the stock load of a missing archive (wp10b0.arc) is fatal: the stock check (FUN_009B12C0) before the load, on the stock path */
        const char *pa = g ? strstr(g, "if (!bg_arc_path(cls, path, sizeof path)) return 0;") : NULL, *ex = g ? strstr(g, "if (!bg_arc_exists(cls, path)) {") : NULL,
                   *ld = g ? strstr(g, "uint32_t a = ((bg_load_fn)") : NULL;
        CHECK(pa && ex && ld && pa < ex && ex < ld && strstr(g, "#define BG_RES_EXISTS 0x9B12C0u") && !strstr(g, "GetFileAttributesA"),
              "D the stock path, then the stock existence check, then the load (m14e: the host's fatal 'Failed open file ... wp10b0.arc')");
        CHECK(g && strstr(g, "bg_exists_sig[] = {0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x9C, 0x24, 0x0C, 0x01, 0x00, 0x00};"), "D FUN_009B12C0's bytes in the glue");
        CHECK(g && strstr(g, "#define BG_PACK_VT 0x10186B0u") && strstr(g, "#define BG_PACK_REMOVE 0x5C6AB0u") && strstr(g, "s.index == pcur"), "D the removal: the stock cBioItemPack_vf7, never the held index");
        CHECK(g && strstr(g, "static volatile int f_bagdlc, f_bagroom;") && strstr(g, "bg_step s = bg_plan(&want, pc, pcur, free_slot);") && strstr(g, "if (s.act == BG_REMOVE && !f_bagroom) return;"),
              "D m15a3: the DLC loads and the removal are LAB (default off); the plan sees the owner's list without the DLC guns");
    }
    CHECK(g && strstr(g, "vp0 != m0 && vp0 != m1"), "D nothing while a cutscene camera is on viewport 0");
    CHECK(w && strstr(w, "#include \"bag1_game.inc\"") && strstr(w, "bag_pass(now);"), "D in the frame");
    CHECK(s && strstr(s, "sub == CP_W1_BAG") && strstr(s, "bag_rx(b, len)"), "D op_ext routes CP_W1_BAG");
    CHECK(p && strstr(p, "#define CP_W1_BAG 26"), "D CP_W1_BAG = 26 (inside CP_W1_EXT_FIRST..LAST)");
    free(g), free(w), free(s), free(p);
}

int main(void)
{
    t_m14a();
    t_rules();
    t_cut13();
    t_dlcfilter();
    t_sources();
    printf("test_bag1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
