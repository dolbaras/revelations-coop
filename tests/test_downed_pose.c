/*
 * Offline test of the downed pose (native/downed1_pose.h): the plans DOWN / KEEP / UP / DIE against a fake engine that models the
 * fields the plans touch (lower state index, input ban, hp, dead flag, the vital object) and the calls they make; and the build facts
 * (entry bytes of the calls, the motion ids and state requests the plan relies on, the state table words, plus the Raid agony facts
 * that ruled it out) against the executable image when it is at hand (REV1_IMAGE, default ../../image/rerev-image.bin).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/downed1_pose.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- fake engine: a flat memory window at 0x00100000 plus the call log ---- */
#define MEM0 0x00100000u
#define MEMN 0x40000u
static uint8_t mem[MEMN];
static char calls[512];
static int ncalls, refuse_state, refuse_play;

static int in_mem(uint32_t a, int n) { return a >= MEM0 && a + (uint32_t)n <= MEM0 + MEMN; }
static uint32_t m32(uint32_t a) { uint32_t v = 0; if (in_mem(a, 4)) memcpy(&v, mem + (a - MEM0), 4); return v; }
static void s32(uint32_t a, uint32_t v) { if (in_mem(a, 4)) memcpy(mem + (a - MEM0), &v, 4); }
static float mf(uint32_t a) { float v = 0; if (in_mem(a, 4)) memcpy(&v, mem + (a - MEM0), 4); return v; }
static void sf(uint32_t a, float v) { if (in_mem(a, 4)) memcpy(mem + (a - MEM0), &v, 4); }
static void logc(const char *s) { size_t l = strlen(calls); snprintf(calls + l, sizeof calls - l, "%s%s", l ? " " : "", s); ncalls++; }
static void clr(void) { calls[0] = 0, ncalls = 0; }

#define UNIT (MEM0 + 0x1000u)
#define VITAL (MEM0 + 0x20000u)
#define MGR (MEM0 + 0x30000u)

static int f_rd(void *u, uint32_t a, void *dst, int n) { (void)u; if (!in_mem(a, n)) return 0; memcpy(dst, mem + (a - MEM0), (size_t)n); return 1; }
/* FUN_006A5290: hp, condition 1, vital state 1 */
static void f_recover(void *u, uint32_t vital, float hp)
{
    (void)u; char b[32]; snprintf(b, sizeof b, "recover%.0f", (double)hp); logc(b);
    uint32_t unit = m32(vital + D1P_V_OWNER);
    sf(unit + D1P_HP_OFF, hp), s32(unit + D1P_COND_OFF, 1), s32(vital + D1P_V_STATE, 1);
}
static void f_ban(void *u, uint32_t unit, int on) { (void)u; logc(on ? "ban1" : "ban0"); uint32_t w = m32(unit + D1P_PSTATE_OFF); s32(unit + D1P_PSTATE_OFF, on ? (w | D1P_BAN_BIT) : (w & ~D1P_BAN_BIT)); }
/* FUN_0049CD20: the current index; the death state's entry = the death 0x6AE530; GetUp's entry sets E1C bit 8 */
static int f_state(void *u, uint32_t mgr, int idx)
{
    (void)u; char b[24]; snprintf(b, sizeof b, "state%X", (unsigned)idx); logc(b);
    if (refuse_state) return 0;
    uint32_t unit = m32(mgr + 0x10u);
    s32(mgr + D1P_MGR_CUR, (uint32_t)idx);
    if (idx == D1P_IDX_DEAD) s32(unit + D1P_FLAGS_OFF, m32(unit + D1P_FLAGS_OFF) | 1u), sf(unit + D1P_HP_OFF, 0.0f);
    if (idx == D1P_IDX_GETUP_B || idx == D1P_IDX_GETUP_F) s32(unit + D1P_FLAGS_OFF, m32(unit + D1P_FLAGS_OFF) | 0x100u);
    return 1;
}
static int f_play(void *u, uint32_t unit, int motion, float speed) { (void)u; (void)unit; char b[24]; snprintf(b, sizeof b, "play%X@%.0f", (unsigned)motion, (double)speed); logc(b); return !refuse_play; }

static const d1p_ops OPS = {NULL, f_rd, f_recover, f_ban, f_state, f_play};

static void fresh(void)
{
    memset(mem, 0, sizeof mem);
    clr(), refuse_state = 0, refuse_play = 0;
    s32(UNIT + D1P_VITAL_OFF, VITAL), s32(UNIT + D1P_MGR_OFF, MGR);
    s32(VITAL, D1P_VITAL_VT), s32(VITAL + D1P_V_OWNER, UNIT);
    s32(MGR + 0x10u, UNIT), s32(MGR + D1P_MGR_CUR, 5);
    sf(UNIT + D1P_HP_OFF, 1.0f), sf(UNIT + D1P_MAX_OFF, 1000.0f);
}

static void t_fall_lie_up(int face_down)
{
    d1p_hum h;
    fresh();
    CHECK(d1p_down(&OPS, UNIT, &h, 1000) == D1P_OK);
    CHECK(!strcmp(calls, "ban1 state2D") && h.phase == D1P_PH_FALLING);   /* the ban, the knock-down; nothing of the Raid agony (it is a T-pose here) */
    CHECK(m32(MGR + D1P_MGR_CUR) == D1P_IDX_FALL && (m32(UNIT + D1P_PSTATE_OFF) & D1P_BAN_BIT));
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 1500) == 0 && ncalls == 0);           /* still falling */
    int idx = face_down ? D1P_IDX_GETUP_F : D1P_IDX_GETUP_B;
    f_state(NULL, MGR, idx);                                            /* the engine: landed, GetUp entered (its motion starts) */
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 1700) == 1);
    CHECK(!strcmp(calls, face_down ? "play316@0" : "play315@0") && h.phase == D1P_PH_LYING && h.frozen == idx);
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 9000) == 0 && ncalls == 0);           /* lying still: KEEP writes nothing */
    s32(UNIT + D1P_PSTATE_OFF, 0);                                      /* a cutscene end cleared the ban */
    CHECK(d1p_keep(&OPS, UNIT, &h, 9100) == 4 && !strcmp(calls, "ban1"));
    clr();
    CHECK(d1p_up(&OPS, UNIT, &h, 0.35f) == D1P_OK);
    CHECK(!strcmp(calls, face_down ? "ban0 play316@1 recover350" : "ban0 play315@1 recover350"));   /* the real get-up, then hp */
    CHECK(h.phase == D1P_PH_UP && mf(UNIT + D1P_HP_OFF) == 350.0f && m32(UNIT + D1P_COND_OFF) == 1 && !(m32(UNIT + D1P_PSTATE_OFF) & D1P_BAN_BIT));
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 9500) == 0 && ncalls == 0);           /* up: KEEP does nothing */
    printf("ok   %s: ban + knock-down 0x2D, frozen at the first frame of the get-up when it lands, KEEP quiet while lying, UP plays the get-up at speed 1\n",
           face_down ? "face down (0x3C / 0x316)" : "on the back (0x3B / 0x315)");
}

static void t_putback(void)
{
    d1p_hum h;
    fresh();
    d1p_down(&OPS, UNIT, &h, 1000);
    f_state(NULL, MGR, D1P_IDX_GETUP_B);
    d1p_keep(&OPS, UNIT, &h, 1100);
    s32(MGR + D1P_MGR_CUR, 0);                                          /* a cutscene took him (EventWaitState) */
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 1500) == 0 && ncalls == 0);          /* not yet: at most once a second */
    CHECK(d1p_keep(&OPS, UNIT, &h, 2200) == 2 && !strcmp(calls, "state3B play315@0") && h.fixes == 1 && h.frozen == D1P_IDX_GETUP_B);
    printf("ok   put back: another state took over -> 0x3B and frozen again, at most once a second\n");

    fresh();
    d1p_down(&OPS, UNIT, &h, 1000);                                     /* the fall never lands (stays 0x2D): after 3 s he lies down directly */
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 3500) == 0 && ncalls == 0);
    CHECK(d1p_keep(&OPS, UNIT, &h, 4000) == 2 && !strcmp(calls, "state3B play315@0") && h.phase == D1P_PH_LYING);

    fresh();
    refuse_state = 1;                                                   /* a locked state machine refuses the fall */
    CHECK(d1p_down(&OPS, UNIT, &h, 1000) == D1P_OK && h.phase == D1P_PH_FALLING);
    refuse_state = 0;
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 1010) == 2 && !strcmp(calls, "state3B play315@0"));   /* no 3 s wait after a refused fall */
    printf("ok   no landing in 3 s, or the fall refused: he lies down directly in 0x3B\n");

    fresh();
    d1p_down(&OPS, UNIT, &h, 1000);
    f_state(NULL, MGR, D1P_IDX_GETUP_B);
    refuse_play = 1;                                                    /* the motion call refused (wrong vtable): stays FALLING, tries again */
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 1100) == 0 && h.phase == D1P_PH_FALLING);
    refuse_play = 0;
    CHECK(d1p_keep(&OPS, UNIT, &h, 1200) == 1 && h.phase == D1P_PH_LYING);
    clr();
    s32(MGR + D1P_MGR_CUR, 5);                                          /* he got up by himself (the motion was not frozen) */
    CHECK(d1p_up(&OPS, UNIT, &h, 0.35f) == D1P_OK && !strcmp(calls, "ban0 recover350"));   /* no get-up motion over another state */
    printf("ok   refused motion call retried; UP plays the get-up only over the frozen state\n");
}

static void t_die(void)
{
    d1p_hum h;
    fresh();
    d1p_down(&OPS, UNIT, &h, 1000);
    f_state(NULL, MGR, D1P_IDX_GETUP_B);
    d1p_keep(&OPS, UNIT, &h, 1100);
    clr();
    CHECK(d1p_die(&OPS, UNIT, &h) == D1P_OK && !strcmp(calls, "ban0 state32"));
    CHECK((m32(UNIT + D1P_FLAGS_OFF) & 1) && mf(UNIT + D1P_HP_OFF) == 0.0f && h.phase == D1P_PH_NONE);
    clr();
    CHECK(d1p_die(&OPS, UNIT, &h) == D1P_DEAD && !strcmp(calls, "ban0"));   /* dead already: no second death */
    clr();
    CHECK(d1p_keep(&OPS, UNIT, &h, 5000) == 0 && ncalls == 0);
    CHECK(d1p_up(&OPS, UNIT, &h, 0.35f) == D1P_DEAD && !strcmp(calls, "ban0"));
    CHECK(d1p_down(&OPS, UNIT, &h, 6000) == D1P_DEAD);
    printf("ok   die: ban off, lower state 0x32 once; a dead unit is never posed, raised or killed again\n");
}

static void t_guards(void)
{
    d1p_hum h;
    fresh();
    s32(UNIT + D1P_MGR_OFF, 0);
    CHECK(d1p_down(&OPS, UNIT, &h, 1) == D1P_NO_MGR && ncalls == 0);
    CHECK(d1p_die(&OPS, UNIT, &h) == D1P_NO_MGR && !strcmp(calls, "ban0"));
    CHECK(d1p_down(&OPS, 0, &h, 1) == D1P_NO_UNIT && d1p_die(&OPS, 0, &h) == D1P_NO_UNIT && d1p_up(&OPS, 0, &h, 0.35f) == D1P_NO_UNIT);
    CHECK(d1p_keep(&OPS, 0, &h, 1) == 0);
    fresh();
    s32(VITAL, 0x1234567u);                                             /* not a cVitalControl: no hp call */
    d1p_down(&OPS, UNIT, &h, 1);
    clr();
    CHECK(d1p_up(&OPS, UNIT, &h, 0.35f) == D1P_NO_UNIT && !strcmp(calls, "ban0"));
    fresh();
    sf(UNIT + D1P_MAX_OFF, 0.0f);
    d1p_down(&OPS, UNIT, &h, 1);
    d1p_up(&OPS, UNIT, &h, 0.35f);
    CHECK(mf(UNIT + D1P_HP_OFF) == 1.0f);                               /* no max: 1 hp, never 0 */
    printf("ok   guards: no state manager / NULL unit / a foreign vital / no max hp\n");
}

/* ---- the build facts in the image ---- */
static uint8_t *img;
static size_t img_n;
static int i_rd(void *u, uint32_t a, void *dst, int n)
{
    (void)u;
    if (a < 0x400000u) return 0;
    size_t o = a - 0x400000u;
    if (o + (size_t)n > img_n) return 0;
    memcpy(dst, img + o, (size_t)n);
    return 1;
}
static int img_has(uint32_t va, const char *hex)
{
    uint8_t want[64]; int n = 0;
    for (const char *p = hex; *p && n < 64;) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) return 0;
        want[n++] = (uint8_t)v, p += 2;
    }
    uint8_t cur[64];
    return i_rd(NULL, va, cur, n) && memcmp(cur, want, (size_t)n) == 0;
}
static void t_image(void)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("(no executable image at %s: the build facts are not compared)\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)n);
    if (!img || fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); CHECK(!"image read"); return; }
    fclose(f);
    img_n = (size_t)n;
    const char *bad = "?";
    CHECK(d1p_check(i_rd, NULL, &bad) == 1);
    if (*bad) printf("first mismatch: %s\n", bad);
    /* the pose */
    CHECK(img_has(0x68B5C0u, "B8 05 03 00 00 C3"));                   /* BrowBack motion 1 (fly) = 0x305 */
    CHECK(img_has(0x68B530u, "B8 07 03 00 00 C3"));                   /* BrowBack motion 2 (land) = 0x307 */
    CHECK(img_has(0x68DEEEu, "81 8E 1C 0E 00 00 00 01 00 00"));       /* GetUpBackward entry: E1C |= 0x100 ... */
    CHECK(img_has(0x56A85Au, "C1 E8 08 A8 01"));                      /* ... and the enemies' target filter skips E1C bit 8 */
    CHECK(img_has(0x68B4C7u, "81 C1 D0 02 00 00 E8 AE 1D 3A 00"));    /* GetUp slot 10: motion ended (0xA2D280) -> the state may end: a frozen motion never ends */
    CHECK(img_has(0x4BBAB4u, "52 6A 00 FF D0 C2 14 00"));             /* slot 0x68 = layer 0 of slot 0x6C, ret 0x14 */
    CHECK(img_has(0x6A8617u, "81 89 3C 0F 00 00 00 00 10 00"));       /* the ban is bit 20 of unit+0xF3C */
    CHECK(img_has(0x6AF209u, "8B 86 3C 0F 00 00 C1 E8 14"));          /* weapon use (0x6AF1E0) reads the ban for a local human */
    /* the Raid agony (why it is not the pose: mode 2 has no motions in the campaign, live T-pose) */
    CHECK(img_has(0x6B6DF7u, "8B 8E 64 13 00 00 E8 0E E4 FE FF"));   /* Raid applier phase 0: ecx = [unit+0x1364]; call 0x6A5210 (enter dying) */
    CHECK(img_has(0x6B6FF0u, "8B 8E 64 13 00 00 E8 E5 E8 FE FF"));   /* phase 4: call 0x6A58E0 (the Raid revive) */
    CHECK(img_has(0x6A5216u, "C7 46 2C 02 00 00 00"));                /* 0x6A5210: vital+0x2C = 2 ... */
    CHECK(img_has(0x6A5239u, "6A 02 E8 40 06 02 00"));                /* ... FUN_006C5880(unit, 2): condition / motion mode 2 */
    CHECK(img_has(0x6A52F3u, "C7 46 2C 01 00 00 00"));                /* 0x6A5290 (revive): vital+0x2C = 1 */
    CHECK(img_has(0x6A58F1u, "F3 0F 59 05 6C 6A 13 01"));             /* Raid revive hp = max * [0x1136A6C] */
    CHECK(img_has(0x1136A6Cu, "CD CC 4C 3E"));                        /* 0.2f (the mod uses 0.35 of the max) */
    CHECK(img_has(0x1136A68u, "00 00 E0 40"));                        /* 7.0f: Raid's self-revive timer vital+0x58 */
    CHECK(img_has(0x6B61DAu, "83 BE 04 0F 00 00 02"));                /* 0x6B5F40: a lethal hit on a dying unit is the death */
    CHECK(img_has(0x56A82Cu, "83 BE 04 0F 00 00 02"));                /* the enemies' target filter skips condition 2 */
    CHECK(img_has(0x6B7B6Cu, "6A 10 68 20 01 00 00 E8 38 AE FE FF")); /* player setup 0x6B7170: alloc 0x120 bytes (the vital) ... */
    CHECK(img_has(0x6B7B81u, "E8 6A CF FE FF"));                      /* ... ctor 0x6A4AF0 ... */
    CHECK(img_has(0x6B7BB2u, "89 BE 64 13 00 00"));                   /* ... stored at unit+0x1364 (campaign humans have one: live, the regeneration, stage3.md item 8) */
    CHECK(img_has(0x6A4B1Au, "C7 45 00 28 54 06 01"));                /* the ctor writes vtbl_cVitalControl 0x1065428 */
    printf("ok   image %s: %d code places + %d state table words of the pose, the fall / get-up facts, the Raid agony facts\n", path, D1P_NSIG, D1P_NDATA);
}

int main(void)
{
    t_fall_lie_up(0);
    t_fall_lie_up(1);
    t_putback();
    t_die();
    t_guards();
    t_image();
    printf("test_downed_pose: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
