/*
 * Offline test of native/lid1.h (sync022 rows W2 / W3): the partner's locker door / weapon box lid as a picture on this PC.
 * Two PCs on a line with a delay, each with the same boxes (a uLockerDoor and a uWeaponBox, the same keys) as models of the game's state machines:
 *   uOpenBox: Closed (motion 0) -> ItemSync -> Opening (motion 1, its enter SPAWNS the content item on that PC) -> Opened (motion 2);
 *   uWeaponBox: Closed (0) -> Opening (1) -> Opened (the weapon GUI opens on that PC) -> Closing (3) -> Closed (0);
 * only a PC's own human moves its state machine (the objects' operator is the own slot). The glue's sender and receiver as lid1_game.inc runs them.
 * Checks: the copy of the box the partner opened shows the opening (layer 0 = 1) and stays CLOSED here (no content spawned, no GUI, no state change by the
 * mirror); the weapon box's close shows the closing; this PC's own human can still open his copy (the stock opening, his own content once); a copy this PC
 * already opened (or is opening) is left alone; a CLOSE on a copy that does not show our OPEN is left alone; another class / no copy; the first sight never
 * sends; the wire. The model before the fix (0.2.1: no mirror) must fail: the copy shows the closed pose (MODEL BEFORE THE FIX FAILS). The image's facts.
 *   sh tests/build.sh -> tests/build/test_lid1.exe
 */
#include <stdio.h>
#include <string.h>

#include "../native/lid1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define NB 2
enum { BOX_LOCKER = 0, BOX_WEAPON = 1 };
static const uint32_t cls_of[NB] = {0x80061200u, 0x80061205u};
typedef struct { int tag, t_state; uint16_t motion; int contents, gui_opens, own_changes, mirror_changes; } Box;
typedef struct { Box b[NB]; ld_tx tx; ld_rx rx; int setter_calls; } Pc;
static Pc pc[2];
static int tick, delay = 3, mirror_on = 1;
static struct { uint8_t m[LD_LEN]; int at, to; } q[64];
static int nq;
static void key_of(int i, uint32_t k4[4]) { k4[0] = 0x00000004u, k4[1] = 0x00000001u, k4[2] = 7, k4[3] = (uint32_t)(20 + i); }

/* the stock enters: they set the motion; Opening of a locker spawns its content here; Opened of a weapon box opens the GUI here */
static void enter(Box *x, int kind, int tag)
{
    x->tag = tag, x->t_state = tick, x->own_changes++;
    switch (tag) {
    case LD_S_CLOSED: x->motion = 0; break;
    case LD_S_OPENING: x->motion = 1; if (kind == BOX_LOCKER) x->contents++; break;
    case LD_S_OPENED: x->motion = 2; if (kind == BOX_WEAPON) x->gui_opens++; break;
    case LD_S_CLOSING: x->motion = 3; break;
    case LD_S_OTHER: break;   /* ItemSync */
    }
}
/* this PC's own human uses box i (the stock flow, its own state machine) */
static int use_at[2][NB], close_at[2][NB];
static void stock(int p)
{
    for (int i = 0; i < NB; i++) {
        Box *x = &pc[p].b[i];
        if (use_at[p][i] == tick && x->tag == LD_S_CLOSED) enter(x, i, i == BOX_LOCKER ? LD_S_OTHER : LD_S_OPENING);
        else if (x->tag == LD_S_OTHER && tick - x->t_state >= 2) enter(x, i, LD_S_OPENING);
        else if (x->tag == LD_S_OPENING && tick - x->t_state >= 30) enter(x, i, LD_S_OPENED);
        else if (x->tag == LD_S_OPENED && i == BOX_WEAPON && close_at[p][i] == tick) enter(x, i, LD_S_CLOSING);
        else if (x->tag == LD_S_CLOSING && tick - x->t_state >= 30) enter(x, i, LD_S_CLOSED);
    }
}
static void glue_tx(int p)
{
    ld_tx_pass(&pc[p].tx);
    for (int i = 0; i < NB; i++) {
        uint32_t k4[4];
        key_of(i, k4);
        ld_msg m;
        if (ld_tx_step(&pc[p].tx, k4, cls_of[i], pc[p].b[i].tag, pc[p].b[i].motion, &m) && mirror_on && nq < 64) ld_enc(q[nq].m, &m), q[nq].at = tick + delay, q[nq].to = 1 - p, nq++;
    }
}
static int copy_present[2] = {1, 1}, copy_cls_ok[2] = {1, 1};
static int last_decide[2];
static void glue_rx(int p, const uint8_t *b)
{
    ld_msg m;
    if (!ld_dec(b, LD_LEN, &m)) { pc[p].rx.bad++; return; }
    pc[p].rx.rx++;
    int i = -1;
    for (int k = 0; k < NB; k++) {
        uint32_t k4[4];
        key_of(k, k4);
        if (!memcmp(k4, m.k4, 16)) i = k;
    }
    int have = i >= 0 && copy_present[p], same = have && cls_of[i] == m.cls && copy_cls_ok[p];
    Box *x = have ? &pc[p].b[i] : NULL;
    int d = ld_rx_decide(&pc[p].rx, &m, have, same, same ? x->tag : LD_S_UNKNOWN, same ? x->motion : 0xFFFF);
    last_decide[p] = d;
    if (d == LD_PLAY) x->motion = m.id, x->mirror_changes += 0, pc[p].setter_calls++;   /* slot 26 only: the picture, never the state */
}
static void step(int n)
{
    for (int k = 0; k < n; k++) {
        tick++;
        for (int j = 0; j < nq;) {
            if (q[j].at <= tick) { glue_rx(q[j].to, q[j].m); q[j] = q[--nq]; }
            else j++;
        }
        for (int p = 0; p < 2; p++) stock(p), glue_tx(p);
    }
}
static void setup(void)
{
    memset(pc, 0, sizeof pc), memset(use_at, 0, sizeof use_at), memset(close_at, 0, sizeof close_at), nq = 0, tick = 0;
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < NB; i++) pc[p].b[i].tag = LD_S_CLOSED, pc[p].b[i].motion = 0;
        ld_tx_reset(&pc[p].tx), copy_present[p] = copy_cls_ok[p] = 1;
    }
    step(2);   /* the first sight of every box (no word) */
}

static int scenarios(void)
{
    int f0 = fails;
    /* 1. A's human opens the locker: B's copy shows the opening and stays closed (no content, no state change) */
    setup();
    use_at[0][BOX_LOCKER] = 10;
    step(60);
    Box *a = &pc[0].b[BOX_LOCKER], *b = &pc[1].b[BOX_LOCKER];
    CHECK(a->tag == LD_S_OPENED && a->contents == 1);
    CHECK(b->tag == LD_S_CLOSED && b->motion == LD_ID_OPEN && b->contents == 0 && b->own_changes == 0 && pc[1].setter_calls == 1);
    CHECK(pc[0].tx.opens == 1 && nq == 0 && pc[1].rx.played_open == 1);
    /* 2. then B's own human opens his copy: the stock opening (his content once); A's copy is Opened: left alone */
    use_at[1][BOX_LOCKER] = tick + 5;
    step(60);
    CHECK(b->tag == LD_S_OPENED && b->contents == 1 && a->contents == 1 && last_decide[0] == LD_R_BUSY && pc[0].setter_calls == 0);
    /* 3. the weapon box: A opens (B shows 1, no GUI on B), A closes (B shows 3), B never left Closed */
    setup();
    use_at[0][BOX_WEAPON] = 10, close_at[0][BOX_WEAPON] = 70;
    Box *wa = &pc[0].b[BOX_WEAPON], *wb = &pc[1].b[BOX_WEAPON];
    step(40);
    CHECK(wa->gui_opens == 1 && wb->gui_opens == 0 && wb->tag == LD_S_CLOSED && wb->motion == LD_ID_OPEN);
    step(80);
    CHECK(wa->tag == LD_S_CLOSED && wb->tag == LD_S_CLOSED && wb->motion == LD_ID_CLOSE && wb->own_changes == 0 && pc[1].rx.played_close == 1);
    /* 4. both open the same locker at once: each copy is busy on the other PC (its own human): left alone, each loots his own */
    setup();
    use_at[0][BOX_LOCKER] = 10, use_at[1][BOX_LOCKER] = 10;
    step(60);
    CHECK(pc[0].b[0].contents == 1 && pc[1].b[0].contents == 1 && pc[0].setter_calls == 0 && pc[1].setter_calls == 0 && pc[0].rx.r_busy == 1 && pc[1].rx.r_busy == 1);
    /* 5. a CLOSE on a copy that does not show our OPEN (the OPEN was lost: B had no copy then) is left alone */
    setup();
    copy_present[1] = 0;
    use_at[0][BOX_WEAPON] = 10, close_at[0][BOX_WEAPON] = 70;
    step(40);
    copy_present[1] = 1;
    step(80);
    CHECK(pc[1].rx.r_nocopy == 1 && pc[1].rx.r_notshown == 1 && wb->motion == 0 && pc[1].setter_calls == 0);
    /* 6. another class at that key here: left alone */
    setup();
    copy_cls_ok[1] = 0;
    use_at[0][BOX_LOCKER] = 10;
    step(60);
    CHECK(pc[1].rx.r_class == 1 && pc[1].b[0].motion == 0 && pc[1].setter_calls == 0);
    /* 7. a box already open at the first sight sends nothing (a PC that came in later) */
    {
        ld_tx t;
        ld_tx_reset(&t);
        uint32_t k4[4] = {1, 2, 3, 4};
        ld_msg m;
        ld_tx_pass(&t);
        CHECK(!ld_tx_step(&t, k4, 0x80061200u, LD_S_OPENING, 1, &m) && t.first_seen == 1);
        ld_tx_pass(&t);
        CHECK(!ld_tx_step(&t, k4, 0x80061200u, LD_S_OPENING, 1, &m));
        ld_tx_pass(&t);
        CHECK(!ld_tx_step(&t, k4, 0x80061200u, LD_S_OPENED, 2, &m));   /* Opened is no word */
    }
    /* 8. the table never overflows: the oldest box gives way */
    {
        static ld_tx t;
        ld_tx_reset(&t);
        ld_msg m;
        for (unsigned i = 0; i < LD_MAX + 10; i++) {
            uint32_t k4[4] = {9, 9, 9, i};
            ld_tx_pass(&t);
            ld_tx_step(&t, k4, 0x80061200u, LD_S_CLOSED, 0, &m);
        }
        CHECK(t.full == 10 && t.first_seen == LD_MAX + 10);
    }
    return fails - f0;
}

/* the build facts lid1_game.inc relies on */
static void image_facts(void)
{
    FILE *f = fopen("../../image/rerev-image.bin", "rb");
    if (!f) {
        printf("image not found: lid1 build facts not checked\n");
        return;
    }
    static uint8_t img[0xE80000];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    CHECK(n > 0xD40000);
#define AT(va) (img + ((va) - 0x400000u))
#define U32(va) ((uint32_t)AT(va)[0] | (uint32_t)AT(va)[1] << 8 | (uint32_t)AT(va)[2] << 16 | (uint32_t)AT(va)[3] << 24)
    /* the enters' motion ids: push <id>; mov ecx, esi; call edx|eax */
    static const struct { uint32_t va; uint8_t id; uint8_t reg; } en[] = {
        {0x66D011u, 0, 0xD2}, {0x66DDD7u, 1, 0xD2}, {0x66D614u, 2, 0xD2}, {0x66D7FCu, 3, 0xD2},   /* uOpenBox Closed / Opening / Opened / Closing */
        {0x67FBC1u, 0, 0xD2}, {0x67FE3Eu, 1, 0xD0}, {0x6801A9u, 3, 0xD2},                        /* uWeaponBox Closed / Opening / Closing */
    };
    for (unsigned i = 0; i < sizeof en / sizeof en[0]; i++) {
        const uint8_t *p = AT(en[i].va);
        CHECK(p[0] == 0x6A && p[1] == en[i].id && p[2] == 0x8B && p[3] == 0xCE && p[4] == 0xFF && p[5] == en[i].reg);
    }
    /* each of those calls goes through slot 26: mov edx|eax, [eax|edx + 0x68] a few bytes before */
    for (unsigned i = 0; i < sizeof en / sizeof en[0]; i++) {
        int found = 0;
        for (int k = 4; k < 30 && !found; k++) {
            const uint8_t *p = AT(en[i].va - (uint32_t)k);
            if (p[0] == 0x8B && (p[1] == 0x50 || p[1] == 0x42) && p[2] == 0x68) found = 1;
        }
        CHECK(found);
    }
    /* slot 26 = 0x4BBA80 -> slot 27 (+0x6C) with layer 0 (push 0; call eax; ret 0x14) in the boxes' vtables (uLockerDoor, uTerragrigiaLockerDoor, uOMCorpseBox,
     * uWeaponBox), slot 27 = 0x6CD200 */
    static const uint32_t vts[] = {0x105E570u, 0x105EBD0u, 0x105E230u, 0x1062278u};
    for (unsigned i = 0; i < 4; i++) CHECK(U32(vts[i] + 0x68) == 0x4BBA80u && U32(vts[i] + 0x6C) == 0x6CD200u);
    CHECK(!memcmp(AT(0x4BBA80u), "\x8b\x54\x24\x14\xf3\x0f\x10\x44\x24\x10\x8b\x01\x8b\x40\x6c", 15) && !memcmp(AT(0x4BBAB5u), "\x6a\x00\xff\xd0\xc2\x14\x00", 7));
    /* the state classes: their enters are slot 5 of the class vtables (uOpenBox Closed / Opening / Opened / Closing, uWeaponBox Closed / Opening / Closing) */
    CHECK(U32(0x105E03Cu + 20) == 0x66CFB0u && U32(0x105E0ACu + 20) == 0x66DD50u && U32(0x105E0E4u + 20) == 0x66D5B0u && U32(0x105E11Cu + 20) == 0x66D7A0u);
    CHECK(U32(0x1062138u + 20) == 0x67FB60u && U32(0x1062168u + 20) == 0x67FDD0u && U32(0x10621C8u + 20) == 0x680150u);
    /* the Closed states' per-frame calls set no motion: no `mov reg, [reg + 0x68]` (8B /r 68 with mod 01) in uOpenBox Closed vf8 / vf9, uWeaponBox Closed vf9 */
    static const struct { uint32_t lo, hi; } fr[] = {{0x66D030u, 0x66D0A0u}, {0x66D0A0u, 0x66D130u}, {0x67FBE0u, 0x67FC40u}};
    for (unsigned i = 0; i < 3; i++) {
        int bad = 0;
        for (uint32_t va = fr[i].lo; va + 3 <= fr[i].hi; va++) {
            const uint8_t *p = AT(va);
            if (p[0] == 0x8B && (p[1] & 0xC7) >= 0x40 && (p[1] & 0xC7) <= 0x47 && (p[1] & 0x07) != 4 && p[2] == 0x68) bad++;
            if (p[0] == 0xFF && (p[1] & 0xF8) == 0x50 && p[1] != 0x54 && p[2] == 0x68) bad++;   /* call [reg + 0x68] */
        }
        CHECK(bad == 0);
    }
    CHECK(U32(0x105E03Cu + 32) == 0x66D030u && U32(0x105E03Cu + 36) == 0x66D0A0u && U32(0x1062138u + 36) == 0x67FBE0u);
    /* the state DTIs lid1_game.inc names (dti-rev1.txt): their constructors pass these names */
    static const struct { uint32_t dti; const char *name; } dt[] = {{0x011797D4u, "ClosedState"}, {0x01179814u, "OpeningState"}, {0x011797F4u, "OpenedState"},
                                                                    {0x01179794u, "ClosingState"}, {0x01179774u, "ItemSyncState"}, {0x0117A1ACu, "ClosedState"},
                                                                    {0x0117A1CCu, "OpeningState"}, {0x0117A18Cu, "OpenedState"}, {0x0117A16Cu, "ClosingState"}};
    for (unsigned i = 0; i < sizeof dt / sizeof dt[0]; i++) {
        uint8_t pat[5] = {0xB9, (uint8_t)dt[i].dti, (uint8_t)(dt[i].dti >> 8), (uint8_t)(dt[i].dti >> 16), (uint8_t)(dt[i].dti >> 24)};
        int named = 0;
        for (size_t o = 0x400; o + 64 < n && !named; o++) {
            if (memcmp(img + o, pat, 5)) continue;
            for (size_t k = o - 20; k < o + 20 && !named; k++) {
                if (img[k] != 0x68) continue;
                uint32_t v;
                memcpy(&v, img + k + 1, 4);
                size_t L = strlen(dt[i].name) + 1;
                if (v > 0x400000u && v - 0x400000u + L < n && !memcmp(img + (v - 0x400000u), dt[i].name, L)) named = 1;
            }
        }
        CHECK(named);
    }
}

int main(void)
{
    image_facts();
    /* the wire */
    ld_msg m = {LD_OPEN, LD_ID_OPEN, 0x80061200u, {1, 2, 3, 4}}, d;
    uint8_t b[LD_LEN];
    CHECK(ld_enc(b, &m) == LD_LEN && ld_dec(b, LD_LEN, &d) && d.kind == LD_OPEN && d.id == 1 && d.cls == 0x80061200u && d.k4[3] == 4);
    CHECK(!ld_dec(b, LD_LEN - 1, &d));
    b[0] = 7;
    CHECK(!ld_dec(b, LD_LEN, &d));
    m.id = 2, ld_enc(b, &m);
    CHECK(!ld_dec(b, LD_LEN, &d));   /* an OPEN with another motion than 1 is no word */
    m.kind = LD_CLOSE, m.id = 3, ld_enc(b, &m);
    CHECK(ld_dec(b, LD_LEN, &d) && d.kind == LD_CLOSE);
    m.cls = 0, ld_enc(b, &m);
    CHECK(!ld_dec(b, LD_LEN, &d));

    int s = scenarios();
    CHECK(s == 0);
    /* the model before the fix (0.2.1): no mirror: the copy of the locker the partner opened shows the closed pose */
    mirror_on = 0;
    int f0 = fails;
    setup();
    use_at[0][BOX_LOCKER] = 10;
    step(60);
    int shown = pc[1].b[BOX_LOCKER].motion == LD_ID_OPEN;
    printf("MODEL BEFORE THE FIX: the partner opens a locker, its copy here shows motion %u: %s\n", pc[1].b[BOX_LOCKER].motion,
           shown ? "the opening (?!)" : "the closed pose (MODEL BEFORE THE FIX FAILS)");
    CHECK(!shown);
    fails = f0 + (shown ? 1 : 0);
    mirror_on = 1;
    printf("test_lid1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
