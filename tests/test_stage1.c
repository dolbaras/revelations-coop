/*
 * test_stage1.c - rev1 live round 12, task 5 (native/stage1.h): the watching PC streams the stage parts of the human it watches.
 * The l12b timeline (lead order: the host's Jill walks from stage 256 into 257 and on to [13]; the guest's Parker stays at [10] in 256 and watches her from
 * cc_must00[3] on): a model of both PCs' stage loaders (wanted list = the floor under the own human, or the partner's codes while watching with the rule).
 * Pass = while the guest watches, its wanted list holds 257's parts wherever Jill is in 257 (no void), and after the reunion it is its own floor again. The
 * model before the fix (the own floor only) never lists 257 on the guest: the check "257 streamed on the watching PC" fails for it.
 */
#include <stdio.h>
#include <string.h>

#include "../native/stage1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

#define C256A 0x1000u   /* (0x100 << 4) | 0 */
#define C256B 0x1001u
#define C257A 0x1010u   /* (0x101 << 4) | 0 */
#define C257B 0x1011u

static int has_stage(const uint32_t c[ST_N], unsigned stage)
{
    for (int i = 0; i < ST_N; i++)
        if (c[i] && (c[i] >> 4) == stage) return 1;
    return 0;
}

/* Jill's floor codes along her walk: 0..9 in 256, 10..12 the border (both), 13..39 in 257, 40..49 back in 256 at [10] */
static void jill_codes(int t, uint32_t c[ST_N])
{
    memset(c, 0, sizeof(uint32_t) * ST_N);
    if (t < 10 || t >= 40) c[0] = C256A, c[1] = C256B;
    else if (t < 13) c[0] = C256B, c[1] = C257A;
    else c[0] = C257A, c[1] = C257B;
}

static int run(int fix, int *void_passes, int *wrong_after)
{
    st_t host, guest;
    st_reset(&host), st_reset(&guest);
    uint8_t wire[ST_MSG_LEN];
    int bad = 0;
    *void_passes = 0, *wrong_after = 0;
    for (int t = 0; t < 50; t++) {
        int watching = t >= 12 && t < 40;   /* the guest watches from [3] (already in 257) to the reunion at [10] */
        /* host (Jill active): its own floor; it sends on change */
        uint32_t hc[ST_N];
        jill_codes(t, hc);
        if (st_own(&host, hc, 0)) {
            st_enc(wire, hc);
            if (fix) st_rx(&guest, wire, sizeof wire);
        }
        /* guest (Parker at [10], 256): own floor = 256; while watching, the partner's codes with the rule */
        uint32_t own[ST_N] = {C256A, C256B, 0, 0}, wanted[ST_N];
        if (st_use_partner(&guest, fix, watching)) memcpy(wanted, guest.partner, sizeof wanted), guest.used++;
        else memcpy(wanted, own, sizeof wanted);
        if (st_own(&guest, own, watching)) bad += watching;   /* never sends while watching */
        if (watching && t >= 13 && !has_stage(wanted, 0x101)) (*void_passes)++;   /* Jill in 257, the guest does not stream it: the void */
        if (!watching && t >= 40 && !(has_stage(wanted, 0x100) && !has_stage(wanted, 0x101))) (*wrong_after)++;
    }
    if (fix && host.tx != 4) bad++;   /* 256 -> border -> 257 -> 256: four changes, nothing more (no resend of unchanged codes) */
    return bad;
}

int main(void)
{
    int v, w;
    CHECK(run(1, &v, &w) == 0 && v == 0 && w == 0, "the rule: 257 streamed on the watching PC wherever Jill is in it; its own floor again after the reunion; no send while watching");
    int pre = run(0, &v, &w);
    (void)pre;
    CHECK(v == 27, "the model before the fix: the watching PC never streams 257 (27 passes of void)");
    printf("before the fix: %d void passes while Jill is in 257\n", v);
    /* codec and guards */
    st_t s;
    st_reset(&s);
    uint32_t z[ST_N] = {0, 0, 0, 0}, bogus[ST_N] = {0x12345678u, 0, 0, 0}, ok[ST_N] = {C257A, 0, 0, 0};
    uint8_t b[ST_MSG_LEN];
    CHECK(!st_own(&s, z, 0) && s.tx == 0, "all-zero codes are never sent");
    CHECK(!st_own(&s, bogus, 0), "a value that is no part code is never sent");
    CHECK(st_own(&s, ok, 0) && !st_own(&s, ok, 0) && s.tx == 1, "the same codes are sent once");
    CHECK(!st_rx(&s, b, ST_MSG_LEN - 1), "a short body is refused");
    st_enc(b, z);
    CHECK(!st_rx(&s, b, sizeof b) && !s.partner_valid, "all-zero partner codes are refused");
    st_enc(b, ok);
    CHECK(st_rx(&s, b, sizeof b) && st_use_partner(&s, 1, 1) && !st_use_partner(&s, 1, 0) && !st_use_partner(&s, 0, 1), "the partner's codes only while watching and with the switch on");
    st_reset(&s);
    CHECK(!st_use_partner(&s, 1, 1) && !s.sent_any, "a new level instance forgets both lists");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
