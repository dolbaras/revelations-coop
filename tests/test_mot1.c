/*
 * test_mot1.c - rev1 live round 14, task 2 (native/mot1.h): a replica enemy plays the motions its owner's PC plays.
 * Model: the owner's zombie changes its layer-0 motion (idle -> walk -> attack -> flinch -> walk) and layer 1 (upper body) while the replica's own state
 * machine never changes (kind 3: FUN_006C9FE0): before the fix the replica keeps its spawn motion the whole time (round 13: "the default pose"); after the
 * fix it plays the owner's motion one line delay later. A lossy, reordering line; a new unit for the key sends everything again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/mot1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void t_codec(void)
{
    mo_msg m = {0x0200, 1, 7, 0x0122, 5, 8.0f, 12.5f, 1.0f}, d;
    uint8_t b[MO_LEN];
    CHECK(mo_enc(b, &m) == MO_LEN && mo_dec(b, MO_LEN, &d) && !memcmp(&m, &d, sizeof m), "A round trip");
    CHECK(!mo_dec(b, MO_LEN - 1, &d), "A short refused");
    m.layer = 3, mo_enc(b, &m);
    CHECK(!mo_dec(b, MO_LEN, &d), "A a layer over 2 refused");
    m.layer = 0, m.speed = 1.0e9f, mo_enc(b, &m);
    CHECK(!mo_dec(b, MO_LEN, &d), "A an absurd speed refused");
}

/* the owner's motions over 300 passes; the line drops every 7th message and delays by 2 passes, delivering two in reverse order now and then */
static int run(int fixed, int *final_match)
{
    mo_t T, R;
    mo_reset(&T), mo_reset(&R);
    static const uint16_t script0[] = {0x0001, 0x0010, 0x0010, 0x0102, 0x0102, 0x0122, 0x0010, 0x0010, 0x0001};
    uint16_t owner[3] = {0, 0xFFFF, 0xFFFF}, replica[3] = {0x0001, 0xFFFF, 0xFFFF};   /* the spawn state's motion */
    struct { uint8_t b[MO_LEN]; int at; } q[256];
    int qn = 0, msgs = 0, match = 0, frames = 0;
    for (int p = 0; p < 300; p++) {
        owner[0] = script0[(p / 30) % 9];
        owner[1] = (p / 45) % 2 ? 0x0201 : 0xFFFF;
        if (p == 150) mo_tx_forget(&T, 0x0200);   /* a new unit (a rearrange): sent again */
        for (unsigned l = 0; l < 3; l++) {
            mo_msg m;
            if (!mo_tx_step(&T, 0x0200, l, owner[l], &m.seq)) continue;
            m.key = 0x0200, m.layer = (uint8_t)l, m.id = owner[l], m.flags = 0, m.blend = 8.0f, m.frame = 0.0f, m.speed = 1.0f;
            if (++msgs % 7 == 0 && qn < 256) { mo_enc(q[qn].b, &m); q[qn].at = p + 2; qn++; mo_enc(q[qn - 1].b, &m); }   /* a retransmission (reliable lane) */
            if (qn < 256) mo_enc(q[qn].b, &m), q[qn].at = p + 2 + (msgs % 11 == 0 ? 3 : 0), qn++;   /* sometimes late: arrives after a newer one */
        }
        for (int i = 0; i < qn;) {
            if (q[i].at <= p) {
                mo_msg d;
                if (mo_dec(q[i].b, MO_LEN, &d) && mo_rx_step(&R, &d) && fixed) replica[d.layer] = d.id;
                memmove(&q[i], &q[i + 1], (size_t)(qn - i - 1) * sizeof q[0]), qn--;
            } else i++;
        }
        if (p >= 10) {
            frames++;
            match += replica[0] == owner[0] && replica[1] == owner[1];
        }
    }
    *final_match = replica[0] == owner[0] && replica[1] == owner[1];
    return frames ? match * 100 / frames : 0;
}

static void t_model(void)
{
    int f0 = 0, f1 = 0;
    int p0 = run(0, &f0), p1 = run(1, &f1);
    printf("B before the fix: the replica plays the owner's motion %d%% of the time; after: %d%%\n", p0, p1);
    CHECK(p0 < 30, "B before the fix the replica keeps its spawn motion (the default pose of round 13)");
    CHECK(p1 > 90 && f1, "B the fix: the replica plays the owner's motions (a line delay of 2-5 passes)");
    mo_t R;
    mo_reset(&R);
    mo_msg a = {1, 0, 5, 0x10, 0, 0, 0, 1}, b2 = a;
    CHECK(mo_rx_step(&R, &a), "C the first is applied");
    b2.seq = 4;
    CHECK(!mo_rx_step(&R, &b2), "C an older sequence is not applied");
    b2.seq = 5;
    CHECK(!mo_rx_step(&R, &b2), "C a duplicate is not applied");
    b2.seq = 6, b2.layer = 1;
    CHECK(mo_rx_step(&R, &b2), "C another layer has its own order");
    a.seq = 4;   /* 255 -> 4 wraps forward */
    R.e[0].seq[0] = 255;
    CHECK(mo_rx_step(&R, &a), "C the sequence wraps");
    mo_t T;
    mo_reset(&T);
    uint8_t s;
    CHECK(mo_tx_step(&T, 9, 0, 0x10, &s) && !mo_tx_step(&T, 9, 0, 0x10, &s) && mo_tx_step(&T, 9, 0, 0x11, &s), "D sent on a change only");
    mo_tx_forget(&T, 9);
    CHECK(mo_tx_step(&T, 9, 0, 0x11, &s), "D a forgotten key sends again");
    for (int k = 0; k < MO_MAX + 3; k++) mo_tx_step(&T, (uint16_t)(100 + k), 0, 1, &s);
    CHECK(T.full > 0, "D a full table is counted, nothing overwritten");
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
    char *g = slurp("../native/motion1_game.inc"), *w = slurp("../native/world1_game.c"), *s = slurp("../native/story1_game.inc"), *p = slurp("../native/coop_proto.h");
    CHECK(g && strstr(g, "rd32(e.unit + WH_U_KIND) != 2") && strstr(g, "le->local"), "E only own-AI units are sent; on the client only its local enemies");
    CHECK(g && strstr(g, "rd32(unit + WH_U_KIND) != 3") && strstr(g, "fn != MO_VF27"), "E only a replica plays it, through the verified slot 27");
    CHECK(w && strstr(w, "#include \"motion1_game.inc\"") && strstr(w, "motion_pass(now);"), "E in the frame");
    CHECK(s && strstr(s, "sub == CP_W1_MOT") && strstr(s, "motion_rx(b, len)"), "E op_ext routes CP_W1_MOT");
    CHECK(p && strstr(p, "#define CP_W1_MOT 25"), "E CP_W1_MOT = 25 (inside CP_W1_EXT_FIRST..LAST)");
    free(g), free(w), free(s), free(p);
}

int main(void)
{
    t_codec();
    t_model();
    t_sources();
    printf("test_mot1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
