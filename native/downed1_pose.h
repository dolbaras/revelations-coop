/* downed1_pose.h - the engine pose of the "downed / revive" rule in Revelations 1: "knocked down, lying, getting up" from the
 * campaign's own states and motions (live check 2026-11-07, main PC: docs/downed-revive.md section 3).
 *
 * Why not the Raid agony: Raid's "dying" (cVitalControl state 2 + FUN_006C5880(unit, 2): condition unit+0xF04 = 2, motion mode
 * unit+0xF08 = 2) works offline as code, but the campaign does not load the motions of mode 2: Parker stood in the bind pose
 * (T-pose, screenshot devscreens/downed/raid-mode2-tpose.png). So the pose uses only what the campaign has:
 *   DOWN : the game's input ban (FUN_006A8610(unit, 1): unit+0xF3C bit 20); the knock-down state 0x2D (DamageBrowState<BrowBack>:
 *          motions 0x305 fly / 0x307 land). When it lands it asks for GetUpBackward 0x3B (or GetUpForward 0x3C, face down); the first
 *          pass that sees that state plays the get-up motion (0x315 / 0x316) again with speed 0: he lies still at its first frame.
 *          GetUp's entry sets unit+0xE1C bit 8, so enemies skip him while he lies (0x56A7D0).
 *   KEEP : each pass while down: the ban again; when another state took over (a cutscene, a grab) he is put back into 0x3B and frozen
 *          (at most once a second).
 *   UP   : the frozen get-up motion is played again at speed 1: the real "getting up", the state then ends by itself (MoveState);
 *          hp = pct * max through the Raid revive function 0x6A5290 (condition 1, the campaign's "danger" limp); the ban off.
 *   DIE  : the ban off; lower state 0x32 (DeadSimpleState<Normal>, the campaign death: its entry kills, the game over follows).
 *
 * Pure C (no Windows, no engine): the glue (downed1.c) gives the memory reader and the engine calls as d1p_ops; the offline test
 * (tests/test_downed_pose.c) checks the bytes against the image and the plans against a fake engine.
 */
#ifndef DOWNED1_POSE_H
#define DOWNED1_POSE_H

#include <stdint.h>
#include <string.h>

#define D1P_VITAL_OFF 0x1364u      /* unit -> cVitalControl* */
#define D1P_MGR_OFF 0xE80u         /* unit -> lower cCharaStateManager* */
#define D1P_MGR_CUR 0x3Cu          /* mgr: index of the current lower state (written by FUN_0049CD20) */
#define D1P_COND_OFF 0xF04u        /* unit condition 0 fine / 1 danger / 2 dying (Raid) */
#define D1P_MODE_OFF 0xF08u        /* unit motion mode */
#define D1P_PSTATE_OFF 0xF3Cu      /* unit state word, bit 20 = input ban */
#define D1P_BAN_BIT 0x100000u
#define D1P_FLAGS_OFF 0xE1Cu       /* bit 0 = dead, bit 8 = getting up (enemies skip) */
#define D1P_HP_OFF 0xE38u
#define D1P_MAX_OFF 0xE40u
#define D1P_V_OWNER 0x0Cu          /* vital -> unit */
#define D1P_V_STATE 0x2Cu          /* vital state: 2 = dying (Raid) */
#define D1P_VITAL_VT 0x1065428u    /* vtbl_cVitalControl */
#define D1P_LOWER_TABLE 0x1137498u /* lower state table: index i -> DTI* at +4*i */
#define D1P_IDX_FALL 0x2D          /* DamageBrowState<BrowBack> */
#define D1P_IDX_GETUP_B 0x3B       /* GetUpBackwardState (lying on the back) */
#define D1P_IDX_GETUP_F 0x3C       /* GetUpForwardState (lying face down) */
#define D1P_IDX_DEAD 0x32          /* DeadSimpleState<Normal> */
#define D1P_MOT_GETUP_B 0x315
#define D1P_MOT_GETUP_F 0x316
#define D1P_VT_PLAY 0x68u          /* unit vtable slot: play a motion on layer 0 */

#define D1P_FN_RECOVER 0x6A5290u   /* thiscall(vital, float hp), ret 4, float in ST0 */
#define D1P_FN_BAN 0x6A8610u       /* thiscall(unit, bool), ret 4 */
#define D1P_FN_STATE 0x49CD20u     /* thiscall(mgr, int idx, int arg, void *args), ret 0xC */
#define D1P_FN_PLAY 0x4BBA80u      /* thiscall(unit, int motion, float blend, float frame, float speed, int flags), ret 0x14 */

typedef struct { const char *name; uint32_t addr; int n; uint8_t b[24]; const char *what; } d1p_sig;

/* code checked before any call: the entries of the four calls and the facts the plan relies on */
#define D1P_NSIG 8
static const d1p_sig d1p_sigs[D1P_NSIG] = {
    {"VitalRecover", D1P_FN_RECOVER, 13, {0x56, 0x8b, 0xf1, 0x6a, 0x00, 0x8d, 0x4e, 0x58, 0xe8, 0xc3, 0x85, 0x23, 0x00},
     "FUN_006A5290(vital, hp): stop the dying timer, hp, condition 1, vital+0x2C = 1 (the Raid revive)"},
    {"InputBan", D1P_FN_BAN, 20, {0x80, 0x7c, 0x24, 0x04, 0x00, 0x74, 0x0d, 0x81, 0x89, 0x3c, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0xc2, 0x04, 0x00},
     "FUN_006A8610(unit, on): unit+0xF3C bit 20 (the game's input ban)"},
    {"ChangeStateIdx", D1P_FN_STATE, 15, {0x56, 0x8b, 0xf1, 0x80, 0x7e, 0x2d, 0x01, 0x75, 0x06, 0x32, 0xc0, 0x5e, 0xc2, 0x0c, 0x00},
     "FUN_0049CD20(mgr, idx, arg, args): change the state by table index"},
    {"PlayMotion", D1P_FN_PLAY, 15, {0x8b, 0x54, 0x24, 0x14, 0xf3, 0x0f, 0x10, 0x44, 0x24, 0x10, 0x8b, 0x01, 0x8b, 0x40, 0x6c},
     "unit vtable slot 0x68 = 0x4BBA80: vf 0x6C(layer 0, motion, blend, frame, speed, flags)"},
    {"GetUpBackMotion", 0x68DF1Eu, 5, {0x68, 0x15, 0x03, 0x00, 0x00}, "GetUpBackwardState entry plays motion 0x315"},
    {"GetUpFwdMotion", 0x68DF9Eu, 5, {0x68, 0x16, 0x03, 0x00, 0x00}, "GetUpForwardState entry plays motion 0x316"},
    {"FallAsksGetUpF", 0x68D7BBu, 7, {0x6a, 0x3c, 0xe8, 0xce, 0xdc, 0xe0, 0xff}, "BrowBack landed: request GetUpForward 0x3C (0x49B490)"},
    {"FallAsksGetUpB", 0x68D7CBu, 7, {0x6a, 0x3b, 0xe8, 0xbe, 0xdc, 0xe0, 0xff}, "BrowBack landed: request GetUpBackward 0x3B"},
};

/* data: the state table entries of the four states used */
#define D1P_NDATA 4
static const struct { const char *name; uint32_t addr, value; } d1p_data[D1P_NDATA] = {
    {"state 0x2D = DamageBrowState<BrowBack>", D1P_LOWER_TABLE + 4u * D1P_IDX_FALL, 0x117ACE0u},
    {"state 0x3B = GetUpBackwardState", D1P_LOWER_TABLE + 4u * D1P_IDX_GETUP_B, 0x117A874u},
    {"state 0x3C = GetUpForwardState", D1P_LOWER_TABLE + 4u * D1P_IDX_GETUP_F, 0x117A994u},
    {"state 0x32 = DeadSimpleState<Normal>", D1P_LOWER_TABLE + 4u * D1P_IDX_DEAD, 0x117ADA0u},
};

typedef int (*d1p_read_fn)(void *u, uint32_t addr, void *dst, int n);   /* 1 = read */

/* 1 when every signature and data word matches; on a mismatch *bad names the first one (the pose stays off, the rule runs without it) */
static inline int d1p_check(d1p_read_fn rd, void *u, const char **bad)
{
    uint8_t cur[24];
    for (int i = 0; i < D1P_NSIG; i++) {
        if (!rd(u, d1p_sigs[i].addr, cur, d1p_sigs[i].n) || memcmp(cur, d1p_sigs[i].b, (size_t)d1p_sigs[i].n) != 0) {
            if (bad) *bad = d1p_sigs[i].name;
            return 0;
        }
    }
    for (int i = 0; i < D1P_NDATA; i++) {
        uint32_t v = 0;
        if (!rd(u, d1p_data[i].addr, &v, 4) || v != d1p_data[i].value) {
            if (bad) *bad = d1p_data[i].name;
            return 0;
        }
    }
    if (bad) *bad = "";
    return 1;
}

/* the engine as the plans see it */
typedef struct {
    void *u;
    d1p_read_fn rd;
    void (*recover)(void *u, uint32_t vital, float hp);
    void (*ban)(void *u, uint32_t unit, int on);
    int (*state)(void *u, uint32_t mgr, int idx);                     /* 1 = changed */
    int (*play)(void *u, uint32_t unit, int motion, float speed);     /* 1 = played (the unit's slot 0x68 is the checked 0x4BBA80) */
} d1p_ops;

static inline uint32_t d1p_rd32(const d1p_ops *o, uint32_t a) { uint32_t v = 0; o->rd(o->u, a, &v, 4); return v; }
static inline float d1p_rdf(const d1p_ops *o, uint32_t a) { float v = 0.0f; o->rd(o->u, a, &v, 4); return v; }

/* the vital object of a unit, or 0 when it is missing or not a cVitalControl owned by this unit */
static inline uint32_t d1p_vital(const d1p_ops *o, uint32_t unit)
{
    if (!unit) return 0;
    uint32_t v = d1p_rd32(o, unit + D1P_VITAL_OFF);
    if (v < 0x10000u || d1p_rd32(o, v) != D1P_VITAL_VT || d1p_rd32(o, v + D1P_V_OWNER) != unit) return 0;
    return v;
}

enum { D1P_OK = 0, D1P_NO_UNIT = 1, D1P_DEAD = 2, D1P_NO_MGR = 3, D1P_REFUSED = 4 };
enum { D1P_PH_NONE = 0, D1P_PH_FALLING, D1P_PH_LYING, D1P_PH_UP };

typedef struct {
    int phase;
    int frozen;          /* the GetUp state index he lies in (0x3B / 0x3C), 0 = none */
    uint32_t t0, tfix;   /* time of DOWN, time of the last put-back */
    unsigned fixes;      /* put-backs (log) */
} d1p_hum;

#define D1P_FALL_MS 3000u    /* no GetUp state this long after DOWN: lie down directly in 0x3B */
#define D1P_FIX_MS 1000u     /* at most one put-back a second */

static inline int d1p_dead(const d1p_ops *o, uint32_t unit) { return (d1p_rd32(o, unit + D1P_FLAGS_OFF) & 1u) != 0; }
static inline int d1p_cur(const d1p_ops *o, uint32_t mgr) { return (int)d1p_rd32(o, mgr + D1P_MGR_CUR); }
static inline int d1p_motion_of(int idx) { return idx == D1P_IDX_GETUP_F ? D1P_MOT_GETUP_F : D1P_MOT_GETUP_B; }

/* lie still in GetUp state idx (already entered): its motion again at speed 0 from frame 0 */
static inline int d1p_freeze(const d1p_ops *o, uint32_t unit, d1p_hum *h, int idx, uint32_t now)
{
    if (!o->play(o->u, unit, d1p_motion_of(idx), 0.0f)) return D1P_REFUSED;
    h->phase = D1P_PH_LYING, h->frozen = idx, h->tfix = now;
    return D1P_OK;
}

/* DOWN: the ban, then the knock-down fall; KEEP freezes him when he lands */
static inline int d1p_down(const d1p_ops *o, uint32_t unit, d1p_hum *h, uint32_t now)
{
    memset(h, 0, sizeof *h);
    if (!unit) return D1P_NO_UNIT;
    if (d1p_dead(o, unit)) return D1P_DEAD;
    uint32_t mgr = d1p_rd32(o, unit + D1P_MGR_OFF);
    if (mgr < 0x10000u) return D1P_NO_MGR;
    o->ban(o->u, unit, 1);
    h->t0 = now, h->tfix = now, h->phase = D1P_PH_FALLING;
    if (!o->state(o->u, mgr, D1P_IDX_FALL)) h->t0 = now - D1P_FALL_MS;   /* refused (a locked machine): KEEP lies him down directly */
    return D1P_OK;
}

/* KEEP (each pass while down). Returns what it did: 0 nothing, 1 froze after the fall, 2 put back, 4 ban again */
static inline int d1p_keep(const d1p_ops *o, uint32_t unit, d1p_hum *h, uint32_t now)
{
    if (!unit || h->phase == D1P_PH_NONE || h->phase == D1P_PH_UP || d1p_dead(o, unit)) return 0;
    uint32_t mgr = d1p_rd32(o, unit + D1P_MGR_OFF);
    if (mgr < 0x10000u) return 0;
    int did = 0;
    if (!(d1p_rd32(o, unit + D1P_PSTATE_OFF) & D1P_BAN_BIT)) o->ban(o->u, unit, 1), did |= 4;
    int cur = d1p_cur(o, mgr);
    if (h->phase == D1P_PH_FALLING) {
        if (cur == D1P_IDX_GETUP_B || cur == D1P_IDX_GETUP_F) {
            if (d1p_freeze(o, unit, h, cur, now) == D1P_OK) did |= 1;
        } else if (cur != D1P_IDX_FALL || (uint32_t)(now - h->t0) >= D1P_FALL_MS) {
            if ((uint32_t)(now - h->tfix) >= D1P_FIX_MS || (uint32_t)(now - h->t0) >= D1P_FALL_MS) {
                h->tfix = now;
                if (o->state(o->u, mgr, D1P_IDX_GETUP_B) && d1p_freeze(o, unit, h, D1P_IDX_GETUP_B, now) == D1P_OK) did |= 2, h->fixes++;
            }
        }
    } else if (h->phase == D1P_PH_LYING && cur != h->frozen && (uint32_t)(now - h->tfix) >= D1P_FIX_MS) {
        h->tfix = now;
        if (o->state(o->u, mgr, D1P_IDX_GETUP_B) && d1p_freeze(o, unit, h, D1P_IDX_GETUP_B, now) == D1P_OK) did |= 2, h->fixes++;
    }
    return did;
}

/* UP: the frozen get-up plays (the real animation), hp = pct * max through the Raid revive function, the ban off */
static inline int d1p_up(const d1p_ops *o, uint32_t unit, d1p_hum *h, float pct)
{
    if (!unit) return D1P_NO_UNIT;
    o->ban(o->u, unit, 0);
    int was = h->phase;
    h->phase = D1P_PH_UP;
    if (d1p_dead(o, unit)) return D1P_DEAD;
    uint32_t mgr = d1p_rd32(o, unit + D1P_MGR_OFF);
    if (was == D1P_PH_LYING && mgr >= 0x10000u && d1p_cur(o, mgr) == h->frozen)
        o->play(o->u, unit, d1p_motion_of(h->frozen), 1.0f);
    uint32_t v = d1p_vital(o, unit);
    float mx = d1p_rdf(o, unit + D1P_MAX_OFF);
    if (v) o->recover(o->u, v, mx > 1.0f ? pct * mx : 1.0f);
    return v ? D1P_OK : D1P_NO_UNIT;
}

/* DIE: the ban off, then the campaign death state (its entry kills: dead flag, hp 0, the death motion; the game over follows) */
static inline int d1p_die(const d1p_ops *o, uint32_t unit, d1p_hum *h)
{
    if (!unit) return D1P_NO_UNIT;
    o->ban(o->u, unit, 0);
    h->phase = D1P_PH_NONE;
    if (d1p_dead(o, unit)) return D1P_DEAD;
    uint32_t mgr = d1p_rd32(o, unit + D1P_MGR_OFF);
    if (mgr < 0x10000u) return D1P_NO_MGR;
    return o->state(o->u, mgr, D1P_IDX_DEAD) ? D1P_OK : D1P_REFUSED;
}

#endif
