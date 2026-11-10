/*
 * Offline test of native/sf1.c (the Episode 1 story mirror: scenario flags E1, the Genesis rate E2, set pieces E6).
 *   Part A: the policy alone (whitelist, host / guest rules, quiet window, held clears, digest, rate, pieces, wire bodies).
 *   Part B: two PCs = two real world1 models (world1.c) + sf1 + a mock engine each (flag store, rate, the 1-2 scan dispatcher of fsm_12.txt, a chain lock),
 *           joined by a simulated line with loss, duplicates and jitter. The mini glue below does what world1_game.c does.
 *   Part C (e1w2 live): the 1-1 start movie (~110 s) after the session barrier opened, the units at 0,0,0: the quiet window must start when this PC's human
 *           is placed (sf1_unplaced / sf1_placed), else the digest goes out before the level start scripts and a guest's level start set crosses as a proposal.
 *           The glue before the fix (window from the epoch start) is run too and must fail the same checks.
 *   sh tests/build.sh   ->  tests/build/test_sf1.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/coop_proto.h"
#include "../native/sf1.h"
#include "../native/world1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ================================================================ part A */
static int A_flags[65536];
static int A_get(void *u, uint32_t id) { (void)u; return id < 65536 ? A_flags[id] : 0; }
static uint32_t A_set_id[512]; static int32_t A_set_v[512]; static int A_nset;
static void A_set(void *u, uint32_t id, int32_t v) { (void)u; if (A_nset < 512) A_set_id[A_nset] = id, A_set_v[A_nset] = v, A_nset++; A_flags[id] = v; }
static uint32_t A_prop_id[512]; static int A_nprop;
static void A_prop(void *u, uint32_t id) { (void)u; if (A_nprop < 512) A_prop_id[A_nprop++] = id; }

/* ep1-probe: the save-flag probe's line (sf1_set_summary): the count, the ids in whitelist order, a hash that tells two bitmaps apart, "..." when cut,
 * a refusing getter (no store at the boot) gives -1 */
static int S_neg(void *u, uint32_t id) { (void)u; (void)id; return -1; }
static void part_a_set_summary(void)
{
    memset(A_flags, 0, sizeof A_flags);
    char l[200];
    uint32_t h0 = 1, h1 = 1, h2 = 1, h3 = 1;
    CHECK(sf1_set_summary(A_get, NULL, &h0, l, sizeof l) == 0 && l[0] == 0 && h0 != 0);
    A_flags[4449] = 1, A_flags[4382] = 1, A_flags[257] = 1, A_flags[9000] = 1, A_flags[3] = 1;   /* 9000 / 3 are not mirrored: not counted */
    CHECK(sf1_set_summary(A_get, NULL, &h1, l, sizeof l) == 3 && !strcmp(l, "257 4382 4449") && h1 != h0);
    A_flags[4382] = 0, A_flags[4399] = 1;
    CHECK(sf1_set_summary(A_get, NULL, &h2, l, sizeof l) == 3 && !strcmp(l, "257 4399 4449") && h2 != h1);
    A_flags[4382] = 1, A_flags[4399] = 0;
    CHECK(sf1_set_summary(A_get, NULL, &h3, NULL, 0) == 3 && h3 == h1);   /* the same set = the same hash; no list wanted */
    for (unsigned i = 0; i < SF1_N; i++) A_flags[sf1_id_at(i)] = 1;
    char s16[16];
    CHECK(sf1_set_summary(A_get, NULL, &h3, s16, sizeof s16) == SF1_N && strlen(s16) < sizeof s16 && strstr(s16, "...") != NULL && !strncmp(s16, "257 4374", 8));
    CHECK(sf1_set_summary(S_neg, NULL, &h3, l, sizeof l) == -1 && h3 == 0);
    memset(A_flags, 0, sizeof A_flags);
}

static void part_a(void)
{
    char sum0[1024];
    int why;
    /* whitelist */
    CHECK(sf1_index(257) == 0 && sf1_index(4374) == 1 && sf1_index(4457) == 84 && sf1_index(4608) == 85 && sf1_index(4736) == 213);
    CHECK(sf1_index(4864) == 214 && sf1_index(5024) == 374 && sf1_index(16725) == 375 && sf1_index(16729) == 379);
    CHECK(sf1_index(0) < 0 && sf1_index(5) < 0 && sf1_index(256) < 0 && sf1_index(258) < 0 && sf1_index(4373) < 0 && sf1_index(4458) < 0 && sf1_index(5025) < 0 && sf1_index(16730) < 0);
    int inv = 1;
    for (unsigned i = 0; i < SF1_N; i++) inv &= sf1_index(sf1_id_at(i)) == (int)i;
    CHECK(inv);
    CHECK(sf1_id_at(SF1_N) == 0);
    CHECK(sf1_layout() != 0 && sf1_layout() == sf1_layout());
    CHECK(SF1_BYTES == 48 && 4 + SF1_BYTES <= W1_EXT_MAX);
    /* every flag the Genesis gates / chain need is mirrored */
    uint32_t need[] = {4398, 4399, 4432, 4658, 4661, 4662, 4663, 4666, 4689, 4705, 4720, 4722, 4730, 4943, 4864, 4899, 4989, 16725};
    int all = 1;
    for (unsigned i = 0; i < sizeof need / sizeof need[0]; i++) all &= sf1_index(need[i]) >= 0;
    CHECK(all);

    sf1_t h, g;
    sf1_init(&h, SF1_HOST), sf1_init(&g, SF1_GUEST);
    CHECK(sf1_local(&h, 4398, 0, 1, 10, &why) == SF1_SKIP && why == SF1_W_NOEPOCH);
    sf1_epoch(&h, 3, 1000), sf1_epoch(&g, 3, 1000);
    CHECK(sf1_in_quiet(&h, 1000) && sf1_in_quiet(&h, 1000 + SF1_QUIET_MS - 1) && !sf1_in_quiet(&h, 1000 + SF1_QUIET_MS));
    /* host: every real change of a mirrored id is sent, also a clear, also in the quiet window */
    CHECK(sf1_local(&h, 4398, 0, 1, 1100, &why) == SF1_SEND);
    CHECK(sf1_local(&h, 4705, 1, 0, 1100, &why) == SF1_SEND);
    CHECK(sf1_local(&h, 4398, 1, 1, 1100, &why) == SF1_SKIP && why == SF1_W_SAME);
    CHECK(sf1_local(&h, 2, 0, 1, 1100, &why) == SF1_SKIP && why == SF1_W_RANGE);          /* the _Host / _Guest rain temporaries stay local */
    CHECK(sf1_local(&h, 9000, 0, 1, 1100, &why) == SF1_SKIP && why == SF1_W_RANGE);
    h.wide = 1;
    CHECK(sf1_local(&h, 9000, 0, 1, 1100, &why) == SF1_SEND);                              /* lab: wide */
    CHECK(sf1_local(&h, 255, 0, 1, 1100, &why) == SF1_SKIP);
    h.wide = 0;
    /* host: proposals */
    CHECK(sf1_proposal(&h, 4398, 1, 0, &why) == SF1_SKIP && why == SF1_W_TOUCHED);        /* the host changed it itself */
    CHECK(sf1_proposal(&h, 4399, 1, 0, &why) == SF1_APPLY);
    CHECK(sf1_proposal(&h, 4399, 1, 1, &why) == SF1_SKIP && why == SF1_W_TOUCHED);        /* applied once only */
    CHECK(sf1_proposal(&h, 4432, 1, 1, &why) == SF1_SKIP && why == SF1_W_SAME);
    CHECK(sf1_proposal(&h, 4432, 1, 0, &why) == SF1_SKIP && why == SF1_W_TOUCHED);        /* "the host had it" counts as decided */
    CHECK(sf1_proposal(&h, 4433, 0, 1, &why) == SF1_SKIP && why == SF1_W_CLEAR);          /* never a clear from the guest */
    CHECK(sf1_proposal(&h, 3, 1, 0, &why) == SF1_SKIP && why == SF1_W_RANGE);
    CHECK(sf1_proposal(&g, 4440, 1, 0, &why) == SF1_SKIP && why == SF1_W_ROLE);
    CHECK(sf1_local(&h, 4399, 1, 0, 1200, &why) == SF1_SEND);                              /* the host's own later clear of an applied proposal goes out */
    /* guest: own sets are proposals, clears stay, the quiet window and the digest defer */
    CHECK(sf1_local(&g, 4440, 0, 1, 1100, &why) == SF1_SKIP && why == SF1_W_QUIET);
    CHECK(sf1_local(&g, 4441, 1, 0, 1100, &why) == SF1_SKIP && why == SF1_W_CLEAR);
    CHECK(sf1_local(&g, 4442, 0, 1, 1000 + SF1_QUIET_MS + 10, &why) == SF1_SKIP && why == SF1_W_QUIET);   /* no digest yet */
    CHECK(!sf1_may_propose(&g, 1000 + SF1_QUIET_MS + 10));
    /* host digest: due once, after the quiet window */
    CHECK(!sf1_digest_due(&h, 1000 + SF1_QUIET_MS - 1));
    CHECK(sf1_digest_due(&h, 1000 + SF1_QUIET_MS) && !sf1_digest_due(&h, 1000 + SF1_QUIET_MS + 1));
    CHECK(!sf1_digest_due(&g, 1000 + SF1_QUIET_MS + 5000));
    memset(A_flags, 0, sizeof A_flags);
    A_flags[4390] = 1, A_flags[4658] = 1, A_flags[16725] = 1;   /* the host's world */
    uint8_t dig[64];
    unsigned dl = sf1_digest_build(&h, A_get, NULL, dig, sizeof dig);
    CHECK(dl == 4 + SF1_BYTES);
    CHECK(sf1_digest_build(&h, A_get, NULL, dig, 20) == 0);
    /* guest world: stale 4374 (own save), own sets 4440 (quiet) and 4442 (before the digest), lacks 4390 / 4658 / 16725, has 4658 too? no */
    memset(A_flags, 0, sizeof A_flags);
    A_flags[4374] = 1, A_flags[4440] = 1, A_flags[4442] = 1, A_flags[16725] = 1;
    A_nset = A_nprop = 0;
    uint8_t bad[64];
    memcpy(bad, dig, dl);
    bad[0] ^= 1;
    CHECK(!sf1_digest_apply(&g, bad, dl, A_get, A_set, A_prop, NULL, 5000));               /* another layout */
    CHECK(!sf1_digest_apply(&g, dig, dl - 1, A_get, A_set, A_prop, NULL, 5000));
    CHECK(!sf1_digest_apply(&h, dig, dl, A_get, A_set, A_prop, NULL, 5000));               /* the host never takes one */
    CHECK(sf1_digest_apply(&g, dig, dl, A_get, A_set, A_prop, NULL, 5000));
    CHECK(A_flags[4390] == 1 && A_flags[4658] == 1 && A_flags[4374] == 0 && A_flags[16725] == 1);
    CHECK(A_flags[4440] == 1 && A_flags[4442] == 1);                                       /* own sets are kept ... */
    CHECK(A_nprop == 2 && ((A_prop_id[0] == 4440 && A_prop_id[1] == 4442) || (A_prop_id[0] == 4442 && A_prop_id[1] == 4440)));   /* ... and proposed */
    CHECK(A_nset == 3 && g.st.digest_set == 2 && g.st.digest_clear == 1 && g.st.digest_prop == 2);
    int nset = A_nset;
    CHECK(sf1_digest_apply(&g, dig, dl, A_get, A_set, A_prop, NULL, 5100) && A_nset == nset);   /* a second copy changes nothing */
    CHECK(sf1_may_propose(&g, 5100));
    CHECK(sf1_local(&g, 4443, 0, 1, 5200, &why) == SF1_SEND);
    CHECK(sf1_local(&g, 4443, 0, 1, 5300, &why) == SF1_SKIP && why == SF1_W_SAME);        /* once per epoch */
    CHECK(sf1_local(&g, 4390, 0, 1, 5300, &why) == SF1_SKIP && why == SF1_W_TOUCHED);     /* the host's digest already spoke about it */
    /* guest stream: sets apply, a clear right after a set is held, a newer set drops the hold */
    CHECK(sf1_remote_stream(&g, 4705, 1, 0, 6000, &why) == SF1_APPLY);
    CHECK(sf1_remote_stream(&g, 4705, 0, 1, 6010, &why) == SF1_LATER);
    uint32_t hid; int32_t hv;
    CHECK(!sf1_tick(&g, 6000 + SF1_HOLD_MS - 1, &hid, &hv));
    CHECK(sf1_tick(&g, 6001 + SF1_HOLD_MS, &hid, &hv) && hid == 4705 && hv == 0 && !sf1_tick(&g, 7000, &hid, &hv));
    CHECK(sf1_remote_stream(&g, 4706, 1, 0, 8000, &why) == SF1_APPLY);
    CHECK(sf1_remote_stream(&g, 4706, 0, 1, 8100, &why) == SF1_LATER);
    CHECK(sf1_remote_stream(&g, 4706, 1, 1, 8200, &why) == SF1_SKIP && why == SF1_W_SAME);   /* the newer set replaces the held clear */
    CHECK(!sf1_tick(&g, 9999, &hid, &hv));
    CHECK(sf1_remote_stream(&g, 4707, 0, 1, 10000, &why) == SF1_APPLY);                   /* a clear without a recent set: at once */
    CHECK(sf1_remote_stream(&g, 4708, 0, 0, 10000, &why) == SF1_SKIP);
    CHECK(sf1_local(&g, 4705, 0, 1, 11000, &why) == SF1_SKIP && why == SF1_W_TOUCHED);    /* a lagging guest cannot re-arm the host's consumed trigger */
    CHECK(sf1_remote_stream(&h, 4705, 1, 0, 6000, &why) == SF1_SKIP && why == SF1_W_ROLE);
    /* hold table full: applied at once */
    for (int i = 0; i < SF1_MAXHOLD + 2; i++) {
        uint32_t id = 4864 + (uint32_t)i;
        sf1_remote_stream(&g, id, 1, 0, 20000, &why);
        int r = sf1_remote_stream(&g, id, 0, 1, 20001, &why);
        CHECK(i < SF1_MAXHOLD ? r == SF1_LATER : (r == SF1_APPLY && why == SF1_W_HOLDFULL));
    }
    int due = 0;
    while (sf1_tick(&g, 30000, &hid, &hv)) due++;
    CHECK(due == SF1_MAXHOLD);
    /* a digest that reaches a guest inside its own quiet window (its load was slower) is kept and applied after it */
    sf1_t g3;
    sf1_init(&g3, SF1_GUEST);
    sf1_epoch(&g3, 9, 50000);
    memset(A_flags, 0, sizeof A_flags);
    A_nset = A_nprop = 0;
    CHECK(sf1_digest_apply(&g3, dig, dl, A_get, A_set, A_prop, NULL, 50100) == 2 && A_nset == 0);
    CHECK(!sf1_digest_kept(&g3, 50000 + SF1_QUIET_MS - 1, A_get, A_set, A_prop, NULL));
    A_flags[4442] = 1;   /* the guest's own start script, inside its quiet window */
    sf1_local(&g3, 4442, 0, 1, 51000, &why);
    CHECK(!sf1_digest_timeout(&g3, 50000 + SF1_QUIET_MS + SF1_DIGEST_WAIT_MS + 1, A_get, A_prop, NULL));   /* a kept digest is no timeout */
    CHECK(sf1_digest_kept(&g3, 50000 + SF1_QUIET_MS, A_get, A_set, A_prop, NULL) && A_flags[4390] == 1 && A_flags[4658] == 1 && A_flags[16725] == 1);
    CHECK(A_nprop == 1 && A_prop_id[0] == 4442 && g3.digest_done && g3.st.digest_rx == 1);
    CHECK(!sf1_digest_kept(&g3, 60000, A_get, A_set, A_prop, NULL));
    /* digest timeout (an older host) */
    sf1_t g2;
    sf1_init(&g2, SF1_GUEST);
    sf1_epoch(&g2, 1, 0);
    memset(A_flags, 0, sizeof A_flags);
    A_flags[4610] = 1;
    sf1_local(&g2, 4610, 0, 1, 100, &why);
    sf1_local(&g2, 4611, 0, 1, 100, &why), A_flags[4611] = 0;   /* set and cleared again by the guest itself: not proposed */
    A_nprop = 0;
    CHECK(!sf1_digest_timeout(&g2, SF1_QUIET_MS + SF1_DIGEST_WAIT_MS - 1, A_get, A_prop, NULL));
    CHECK(sf1_digest_timeout(&g2, SF1_QUIET_MS + SF1_DIGEST_WAIT_MS, A_get, A_prop, NULL) && A_nprop == 1 && A_prop_id[0] == 4610);
    CHECK(!sf1_digest_timeout(&g2, SF1_QUIET_MS + SF1_DIGEST_WAIT_MS + 5, A_get, A_prop, NULL));
    /* epoch change forgets everything */
    sf1_epoch(&g, 4, 40000);
    CHECK(!g.digest_done && g.nhold == 0 && sf1_local(&g, 4390, 0, 1, 40001, &why) == SF1_SKIP && why == SF1_W_QUIET);
    sf1_epoch(&g, 4, 50000);
    CHECK(g.epoch_ms == 40000);   /* the same epoch again: no-op */

    /* (e1w2) the window waits for the own human's placement */
    sf1_t qh, pg;
    sf1_init(&qh, SF1_HOST), sf1_init(&pg, SF1_GUEST);
    sf1_unplaced(&qh);
    CHECK(!qh.unplaced);   /* no epoch: nothing to wait for */
    sf1_epoch(&qh, 1, 1000), sf1_unplaced(&qh);
    sf1_epoch(&pg, 1, 1000), sf1_unplaced(&pg);
    CHECK(sf1_in_quiet(&qh, 1000 + SF1_QUIET_MS + 60000) && !sf1_digest_due(&qh, 1000 + SF1_QUIET_MS + 60000));   /* a 60 s movie: no digest yet */
    CHECK(sf1_local(&pg, 4449, 0, 1, 61000, &why) == SF1_SKIP && why == SF1_W_QUIET && sf1_local(&qh, 4449, 0, 1, 61000, &why) == SF1_SEND);
    CHECK(!sf1_digest_timeout(&pg, 1000 + SF1_QUIET_MS + SF1_DIGEST_WAIT_MS + 60000, A_get, A_prop, NULL));
    CHECK(sf1_placed(&qh, 110000) == 1 && sf1_placed(&qh, 110016) == 0 && qh.quiet_ms == 110000);
    CHECK(sf1_in_quiet(&qh, 110000 + SF1_QUIET_MS - 1) && !sf1_in_quiet(&qh, 110000 + SF1_QUIET_MS));
    CHECK(!sf1_digest_due(&qh, 110000 + SF1_QUIET_MS - 1) && sf1_digest_due(&qh, 110000 + SF1_QUIET_MS));
    CHECK(sf1_placed(&pg, 110300) == 1);
    CHECK(!sf1_digest_timeout(&pg, 110300 + SF1_QUIET_MS + SF1_DIGEST_WAIT_MS - 1, A_get, A_prop, NULL));
    sf1_epoch(&qh, 2, 200000);
    CHECK(!qh.unplaced && qh.quiet_ms == 200000 && sf1_placed(&qh, 200100) == 0);   /* a new epoch: placed unless the glue says otherwise */
    sf1_unplaced(&qh);
    CHECK(sf1_in_quiet(&qh, 200000 + SF1_PLACE_MAX_MS + SF1_QUIET_MS - 1) && !sf1_in_quiet(&qh, 200000 + SF1_PLACE_MAX_MS + SF1_QUIET_MS));   /* the cap */
    CHECK(sf1_digest_due(&qh, 200000 + SF1_PLACE_MAX_MS + SF1_QUIET_MS));
    CHECK(sf1_placed(&qh, 200000 + SF1_PLACE_MAX_MS + 9000) == 1 && qh.quiet_ms == 200000 + SF1_PLACE_MAX_MS && !sf1_in_quiet(&qh, 200000 + SF1_PLACE_MAX_MS + 9000));
    sf1_summary(&pg, sum0, sizeof sum0);
    CHECK(strstr(sum0, "not placed") == NULL);
    sf1_epoch(&pg, 2, 300000), sf1_unplaced(&pg), sf1_summary(&pg, sum0, sizeof sum0);
    CHECK(strstr(sum0, "not placed") != NULL);

    /* rate */
    sf1_t rh, rg;
    sf1_init(&rh, SF1_HOST), sf1_init(&rg, SF1_GUEST);
    sf1_epoch(&rh, 1, 0), sf1_epoch(&rg, 1, 0);
    int kind, val, outv, send;
    CHECK(!sf1_rate_poll(&rh, 0, &kind, &val) && sf1_rate_value(&rh) == 0);   /* baseline */
    CHECK(sf1_rate_poll(&rh, 18, &kind, &val) && kind == SF1_RATE_ABS && val == 18);
    CHECK(!sf1_rate_poll(&rh, 18, &kind, &val));
    CHECK(!sf1_rate_poll(&rh, 101, &kind, &val) && !sf1_rate_poll(&rh, -1, &kind, &val));
    CHECK(!sf1_rate_poll(&rg, 5, &kind, &val));
    CHECK(sf1_rate_poll(&rg, 23, &kind, &val) && kind == SF1_RATE_DELTA && val == 18);
    CHECK(!sf1_rate_poll(&rg, 0, &kind, &val));                                /* the game lowered it: nothing */
    CHECK(sf1_rate_remote(&rh, SF1_RATE_DELTA, 18, 18, &outv, &send) && outv == 36 && send);
    CHECK(!sf1_rate_poll(&rh, 36, &kind, &val));                               /* the write is not seen as a change of its own */
    CHECK(sf1_rate_remote(&rh, SF1_RATE_DELTA, 90, 36, &outv, &send) && outv == 100 && send);   /* clamp as FUN_008B9D30 */
    CHECK(!sf1_rate_remote(&rh, SF1_RATE_DELTA, 5, 100, &outv, &send) && !send);
    CHECK(!sf1_rate_remote(&rh, SF1_RATE_ABS, 50, 10, &outv, &send));          /* the host never takes an absolute */
    CHECK(!sf1_rate_remote(&rh, SF1_RATE_DELTA, -3, 10, &outv, &send) && !sf1_rate_remote(&rh, SF1_RATE_DELTA, 0, 10, &outv, &send));
    CHECK(sf1_rate_remote(&rg, SF1_RATE_ABS, 36, 0, &outv, &send) && outv == 36 && !send);
    CHECK(!sf1_rate_poll(&rg, 36, &kind, &val));
    CHECK(!sf1_rate_remote(&rg, SF1_RATE_ABS, 36, 36, &outv, &send));
    CHECK(!sf1_rate_remote(&rg, SF1_RATE_ABS, 101, 36, &outv, &send) && !sf1_rate_remote(&rg, SF1_RATE_DELTA, 5, 36, &outv, &send));

    /* pieces */
    sf1_t ph;
    sf1_init(&ph, SF1_HOST);
    uint32_t k1[4] = {1, 2, 3, 4}, k2[4] = {1, 2, 3, 5};
    CHECK(!sf1_piece_local(&ph, k1, 0x80061600u));                             /* no epoch */
    sf1_epoch(&ph, 1, 0);
    CHECK(sf1_piece_class(0x80061600u) && !sf1_piece_class(0x80061100u) && !sf1_piece_class(0x80061001u));
    CHECK(!sf1_piece_local(&ph, k1, 0x80061100u));                             /* a ship window: not in the verified table */
    CHECK(sf1_piece_local(&ph, k1, 0x80061600u) && !sf1_piece_local(&ph, k1, 0x80061600u));
    CHECK(!sf1_piece_remote(&ph, k1, 0x80061600u) && sf1_piece_known(&ph, k1) == 3);   /* both broke it: nothing more */
    CHECK(sf1_piece_remote(&ph, k2, 0x80061600u) && !sf1_piece_remote(&ph, k2, 0x80061600u));
    CHECK(!sf1_piece_local(&ph, k2, 0x80061600u));                             /* our own write broke it: no echo */
    sf1_epoch(&ph, 2, 10);
    CHECK(sf1_piece_known(&ph, k1) == 0 && sf1_piece_local(&ph, k1, 0x80061600u));

    /* wire */
    uint8_t b[32];
    uint32_t id, kk[4], cls;
    int32_t v;
    CHECK(sf1_enc_prop(b, sizeof b, 4943, 1) == 8 && sf1_dec_prop(b, 8, &id, &v) && id == 4943 && v == 1 && !sf1_dec_prop(b, 7, &id, &v));
    CHECK(sf1_enc_rate(b, sizeof b, SF1_RATE_DELTA, 18) == 3 && sf1_dec_rate(b, 3, &kind, &val) && kind == SF1_RATE_DELTA && val == 18);
    b[0] = 7;
    CHECK(!sf1_dec_rate(b, 3, &kind, &val));
    CHECK(sf1_enc_piece(b, sizeof b, k1, 0x80061600u) == 20 && sf1_dec_piece(b, 20, kk, &cls) && !memcmp(kk, k1, sizeof kk) && cls == 0x80061600u && !sf1_dec_piece(b, 19, kk, &cls));
    CHECK(sf1_enc_piece(b, 10, k1, 1) == 0 && sf1_enc_prop(b, 4, 1, 1) == 0);
    char sum[1024];
    sf1_summary(&h, sum, sizeof sum);
    CHECK(strstr(sum, "HOST") != NULL);
    part_a_set_summary();
}

/* ================================================================ part B: two PCs */
typedef struct { unsigned sub_lane; unsigned kind; uint8_t body[CP_WORLD_MAX]; unsigned len; uint32_t due; int used; int lane; } Pkt;
typedef struct { Pkt q[2048]; unsigned rng; int loss, dup, jitter, base; uint32_t sent; } Link;
static uint32_t g_now;
static unsigned rnd(unsigned *s) { unsigned x = *s ? *s : 2463534242u; x ^= x << 13, x ^= x >> 17, x ^= x << 5; return *s = x; }
static void link_put(Link *l, int lane, unsigned kind, const uint8_t *b, unsigned len)
{
    l->sent++;
    if ((int)(rnd(&l->rng) % 100) < l->loss) return;
    int copies = (int)(rnd(&l->rng) % 100) < l->dup ? 2 : 1;
    for (int c = 0; c < copies; c++)
        for (int i = 0; i < 2048; i++)
            if (!l->q[i].used) {
                Pkt *p = &l->q[i];
                p->used = 1, p->lane = lane, p->kind = kind, p->len = len, memcpy(p->body, b, len);
                p->due = g_now + (uint32_t)l->base + (l->jitter ? rnd(&l->rng) % (unsigned)(l->jitter + 1) : 0);
                break;
            }
}

#define NSAMPLES 6
typedef struct PC {
    int role;
    w1_t w;
    sf1_t s;
    Link *tx;
    int8_t fl[20000];
    int rate;
    int in_ops;                    /* the glue is applying something of the other PC (the hook does not report it) */
    int scans;                     /* scenes played here */
    int chain_hp, chain_seen_broken;
    uint32_t chain_k[4];
    int gate50, gate100;           /* the main FSM nodes "50%" / "100%" passed here */
    uint32_t ext_tx;
    uint32_t placed_at;            /* part C: the own human stands in the level from this time on (0 = always) */
    int movie, started, start_saw_set, prop4449, dig_built, dig_had_4449, dig_before_place;
} PC;
static int g_place_gate = 1;      /* part C: 0 = the glue before the e1w2 fix (the window starts at the epoch) */
static int placed(const PC *p) { return (int32_t)(g_now - p->placed_at) >= 0; }

static int pc_get(void *u, uint32_t id) { PC *p = u; return id < 20000 ? p->fl[id] : 0; }
/* the engine's SetScenarioFlag: the hook reads the old value, calls the store, reports (only when not inside ops) */
static void eng_set(PC *p, uint32_t id, int v)
{
    int old = p->fl[id];
    p->fl[id] = (int8_t)v;
    if (p->in_ops) return;
    int why;
    if (sf1_local(&p->s, id, old, v, g_now, &why) != SF1_SEND) return;
    if (p->role == SF1_HOST) w1_local_flag(&p->w, id, v);
    else {
        uint8_t b[8];
        w1_send_ext(&p->w, CP_W1_SFPROP, b, sf1_enc_prop(b, sizeof b, id, v));
        p->ext_tx++;
        if (id == 4449) p->prop4449++;
    }
}
static void ops_set(PC *p, uint32_t id, int v) { p->in_ops = 1, eng_set(p, id, v), p->in_ops = 0; }
static void dg_set(void *u, uint32_t id, int32_t v) { ops_set(u, id, v); }
static void dg_prop(void *u, uint32_t id)
{
    PC *p = u;
    uint8_t b[8];
    w1_send_ext(&p->w, CP_W1_SFPROP, b, sf1_enc_prop(b, sizeof b, id, 1));
    p->ext_tx++;
    if (id == 4449) p->prop4449++;
}
static void send_rate(PC *p, int kind, int v)
{
    uint8_t b[4];
    w1_send_ext(&p->w, CP_W1_SFRATE, b, sf1_enc_rate(b, sizeof b, kind, v));
    p->ext_tx++;
}

static void cb_send(void *u, int lane, unsigned kind, const uint8_t *b, unsigned len) { link_put(((PC *)u)->tx, lane, kind, b, len); }
static void cb_flag(void *u, uint32_t id, int32_t v)
{
    PC *p = u;
    int why;
    if (p->role == SF1_HOST) {   /* an older guest's FLAG: a proposal */
        if (sf1_proposal(&p->s, id, v, p->fl[id], &why) == SF1_APPLY) ops_set(p, id, v), w1_note_flag(&p->w, id, v);
        return;
    }
    if (sf1_remote_stream(&p->s, id, v, p->fl[id], g_now, &why) == SF1_APPLY) ops_set(p, id, v);
}
static void cb_ext(void *u, unsigned sub, const uint8_t *b, unsigned len)
{
    PC *p = u;
    int why;
    if (sub == CP_W1_SFPROP) {
        uint32_t id;
        int32_t v;
        if (p->role == SF1_HOST && sf1_dec_prop(b, len, &id, &v) && sf1_proposal(&p->s, id, v, p->fl[id], &why) == SF1_APPLY) {
            ops_set(p, id, v);
            w1_note_flag(&p->w, id, v);   /* into the host's table, not sent (the guest has it): the host's later clear is a newer version */
        }
    } else if (sub == CP_W1_SFDIG) {
        sf1_digest_apply(&p->s, b, len, pc_get, dg_set, dg_prop, p, g_now);
    } else if (sub == CP_W1_SFRATE) {
        int kind, v, o, send;
        if (sf1_dec_rate(b, len, &kind, &v) && sf1_rate_remote(&p->s, kind, v, p->rate, &o, &send)) {
            p->rate = o;
            if (send) send_rate(p, SF1_RATE_ABS, o);
        }
    } else if (sub == CP_W1_PIECE) {
        uint32_t k[4], cls;
        if (sf1_dec_piece(b, len, k, &cls) && sf1_piece_remote(&p->s, k, cls) && !memcmp(k, p->chain_k, sizeof k) && p->chain_hp > 0) p->chain_hp = 0;   /* the applier: hp = 0 */
    }
}

static void pc_init(PC *p, int role, Link *tx)
{
    memset(p, 0, sizeof *p);
    p->role = role, p->tx = tx;
    w1_ops o;
    memset(&o, 0, sizeof o);
    o.user = p, o.send = cb_send, o.flag_set = cb_flag, o.ext = cb_ext;
    w1_init(&p->w, role == SF1_HOST ? W1_HOST : W1_CLIENT, role == SF1_HOST ? 0 : 1, &o);
    w1_set_flag_raw(&p->w, 1);
    sf1_init(&p->s, role);
    p->chain_hp = 1;
    p->chain_k[0] = 0x100, p->chain_k[1] = 1, p->chain_k[2] = 10, p->chain_k[3] = 0;
}

/* the "FSMs" of this PC (every frame): the 1-2 dispatcher of fsm_12.txt line 1650 + the scene main / sub of 12_030_0, the main FSM gates, the 1-1 keyevent */
static void fsm(PC *p)
{
    if (p->movie && !p->started && placed(p)) {   /* part C: 11_00 starts after the movie: SF 4449 (both PCs, each its own script) */
        p->started = 1, p->start_saw_set = p->fl[4449];
        eng_set(p, 4449, 1);
    }
    for (int i = 0; i < NSAMPLES; i++) {
        uint32_t trig = 4705 + (uint32_t)i, counted = 4689 + (uint32_t)i;
        if (!p->fl[trig]) continue;
        p->scans++;
        eng_set(p, trig, 0);                 /* n5 flgonoff */
        if (!p->fl[counted]) {
            eng_set(p, counted, 1);          /* n6 */
            eng_set(p, 4730, 1);             /* the sub: rate add is a stub; thresholds from the LOCAL rate */
            if (p->rate > 20 && !p->fl[4658]) eng_set(p, 4658, 1);
            if (p->rate > 50 && !p->fl[4661]) eng_set(p, 4661, 1);
            if (p->rate > 60 && !p->fl[4662]) eng_set(p, 4662, 1);
            if (p->rate > 70 && !p->fl[4663]) eng_set(p, 4663, 1);
            if (p->rate >= 100 && !p->fl[4666]) eng_set(p, 4666, 1);
        }
    }
    if (p->fl[4661]) p->gate50 = 1;
    if (p->fl[4666]) p->gate100 = 1;
    if (p->chain_hp <= 0 && !p->fl[4398]) eng_set(p, 4398, 1);   /* keyevent n4 unlock (ArgCheckCharaState 2) */
}

/* the scanner: only the PC whose human holds it (Jill) */
static void scan(PC *p, int sample, int qty)
{
    p->rate += qty;
    if (p->rate > 100) p->rate = 100;           /* FUN_008B9D30 */
    eng_set(p, 4705 + (uint32_t)sample, 1);      /* cGenesis..._vf17: SetScenarioFlag(unit+0xFB4, 1) */
}

static void glue_frame(PC *p, unsigned epoch)
{
    w1_set_epoch(&p->w, epoch);
    int fresh = !p->s.epoch_valid || p->s.epoch != epoch;
    sf1_epoch(&p->s, epoch, g_now);
    if (g_place_gate) {   /* story_epoch: the window waits for the own human */
        if (fresh && !placed(p)) sf1_unplaced(&p->s);
        if (p->s.unplaced && placed(p)) sf1_placed(&p->s, g_now);
    }
    int kind, v;
    if (sf1_rate_poll(&p->s, p->rate, &kind, &v)) send_rate(p, kind, v);   /* before the events of this frame */
    if (p->role == SF1_HOST && sf1_digest_due(&p->s, g_now)) {
        uint8_t b[W1_EXT_MAX];
        unsigned n = sf1_digest_build(&p->s, pc_get, p, b, sizeof b);
        p->dig_built++, p->dig_had_4449 = p->fl[4449], p->dig_before_place = !placed(p);
        w1_send_ext(&p->w, CP_W1_SFDIG, b, n), p->ext_tx++;
        if (sf1_rate_value(&p->s) >= 0) send_rate(p, SF1_RATE_ABS, sf1_rate_value(&p->s));
    }
    if (p->role == SF1_GUEST) {
        sf1_digest_kept(&p->s, g_now, pc_get, dg_set, dg_prop, p);
        sf1_digest_timeout(&p->s, g_now, pc_get, dg_prop, p);
        uint32_t id;
        int32_t hv;
        while (sf1_tick(&p->s, g_now, &id, &hv)) ops_set(p, id, hv);
    }
    /* the set piece watcher */
    if (p->chain_hp <= 0 && !p->chain_seen_broken) {
        p->chain_seen_broken = 1;
        if (sf1_piece_local(&p->s, p->chain_k, 0x80061600u)) {
            uint8_t b[20];
            w1_send_ext(&p->w, CP_W1_PIECE, b, sf1_enc_piece(b, sizeof b, p->chain_k, 0x80061600u)), p->ext_tx++;
        }
    }
    fsm(p);
    w1_tick(&p->w, g_now);
}

static void deliver(Link *l, PC *to)
{
    for (int i = 0; i < 2048; i++) {
        Pkt *k = &l->q[i];
        if (!k->used || (int32_t)(g_now - k->due) < 0) continue;
        k->used = 0;
        w1_recv(&to->w, k->lane, k->kind, k->body, k->len);
    }
}

static uint32_t g_guest_late;   /* the guest's level instance starts this much later (a slower load) */
static void run(PC *h, PC *g, Link *hl, Link *gl, unsigned epoch, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 16) {
        g_now += 16;
        deliver(hl, g), deliver(gl, h);
        glue_frame(h, epoch);
        if (t >= g_guest_late || g->w.epoch_valid) glue_frame(g, epoch);
        else w1_tick(&g->w, g_now);
    }
}

static int same_flags(const PC *a, const PC *b, uint32_t *first)
{
    for (unsigned i = 0; i < SF1_N; i++) {
        uint32_t id = sf1_id_at(i);
        if (a->fl[id] != b->fl[id]) {
            if (first) *first = id;
            return 0;
        }
    }
    return 1;
}

static Link L1, L2;
static PC H, G;

static void scenario(int scanner_is_guest, int loss, unsigned seed)
{
    memset(&L1, 0, sizeof L1), memset(&L2, 0, sizeof L2);
    L1.rng = seed, L2.rng = seed * 7 + 1;
    L1.loss = L2.loss = loss, L1.dup = L2.dup = 5, L1.jitter = L2.jitter = 40, L1.base = L2.base = 20;
    pc_init(&H, SF1_HOST, &L1), pc_init(&G, SF1_GUEST, &L2);
    g_now = 100000;
    /* the guest's own save differs (Continue): stale 4374 here, the host has 4390 */
    H.fl[4390] = 1, G.fl[4374] = 1;
    G.rate = 40;                     /* a stale rate of the guest's own profile: the host's value wins */
    g_guest_late = (seed & 1) ? 2000 : 0;
    run(&H, &G, &L1, &L2, 5, 8000);  /* load, quiet window, digest */
    g_guest_late = 0;
    uint32_t first = 0;
    CHECK(same_flags(&H, &G, &first));
    if (first) printf("  (scenario %d loss %d: first differing id %u)\n", scanner_is_guest, loss, first);
    CHECK(G.fl[4390] == 1 && G.fl[4374] == 0 && G.rate == 0 && H.rate == 0);
    /* 1-2: the scanner scans six samples of 18 (like 12_030_0_sub mRate=18), 1.5 s apart */
    PC *sc = scanner_is_guest ? &G : &H;
    for (int i = 0; i < NSAMPLES; i++) {
        scan(sc, i, 18);
        run(&H, &G, &L1, &L2, 5, 1500);
    }
    run(&H, &G, &L1, &L2, 5, 5000);
    CHECK(H.rate == 100 && G.rate == 100);
    CHECK(H.gate50 && G.gate50 && H.gate100 && G.gate100);   /* the story gates of 1-2 pass on both PCs */
    CHECK(H.fl[4666] == 1 && G.fl[4666] == 1 && H.fl[4661] == 1 && G.fl[4661] == 1);
    first = 0;
    CHECK(same_flags(&H, &G, &first));
    if (first) printf("  (scenario %d loss %d: first differing id %u: host %d guest %d)\n", scanner_is_guest, loss, first, H.fl[first], G.fl[first]);
    for (int i = 0; i < NSAMPLES; i++) CHECK(H.fl[4705 + i] == 0 && G.fl[4705 + i] == 0 && H.fl[4689 + i] == 1 && G.fl[4689 + i] == 1);
    CHECK(sc->scans == NSAMPLES);
    CHECK(H.scans <= NSAMPLES && G.scans <= NSAMPLES);   /* no scan scene plays twice on a PC (the host's echo of a proposal once replayed them on the guest) */
    /* 1-1 chain: broken by one human's blow on one PC only */
    PC *br = scanner_is_guest ? &H : &G;
    br->chain_hp = 0;
    run(&H, &G, &L1, &L2, 5, 3000);
    CHECK(H.chain_hp == 0 && G.chain_hp == 0 && H.fl[4398] == 1 && G.fl[4398] == 1);
    CHECK(H.s.st.piece_tx + G.s.st.piece_tx == 1);   /* one announcement, no echo */
    /* a lagging guest event re-sets a consumed trigger by itself: refused (the host consumed it) and the host stays */
    int hscans = H.scans;
    uint32_t refused = H.s.st.prop_refused, gsent = G.s.st.prop_sent;
    G.fl[4705] = 0;
    eng_set(&G, 4705, 1);
    run(&H, &G, &L1, &L2, 5, 3000);
    CHECK(H.scans == hscans && H.fl[4705] == 0);                 /* the host's dispatcher never saw it again */
    CHECK(G.s.st.prop_sent == gsent && H.s.st.prop_refused == refused);   /* not even proposed: the host had spoken about 4705 */
    CHECK(H.fl[4689] == 1 && H.rate == 100);
    /* quiet: no ping-pong, nothing keeps going over the line */
    uint32_t tx1 = H.ext_tx + G.ext_tx, fl1 = H.w.st.tx_ev + G.w.st.tx_ev;
    run(&H, &G, &L1, &L2, 5, 20000);
    CHECK(H.ext_tx + G.ext_tx == tx1);
    CHECK(H.w.st.tx_ev + G.w.st.tx_ev == fl1);
    CHECK(w1_pending(&H.w) == 0 && w1_pending(&G.w) == 0);
    first = 0;
    CHECK(same_flags(&H, &G, &first));
    /* a new level instance: the old epoch's events never touch it; the digest repeats */
    H.fl[4864] = 1;
    run(&H, &G, &L1, &L2, 6, 6000);
    CHECK(G.fl[4864] == 1 && H.s.st.digest_tx == 2);
}

/* part C: the 1-1 start movie. Returns the number of the discriminating checks that failed (0 = the behaviour wanted). */
static int movie(int guest_first, int loss, unsigned seed)
{
    memset(&L1, 0, sizeof L1), memset(&L2, 0, sizeof L2);
    L1.rng = seed, L2.rng = seed * 5 + 3;
    L1.loss = L2.loss = loss, L1.dup = L2.dup = 5, L1.jitter = L2.jitter = 40, L1.base = L2.base = 20;
    pc_init(&H, SF1_HOST, &L1), pc_init(&G, SF1_GUEST, &L2);
    g_now = 300000;
    H.movie = G.movie = 1;
    H.placed_at = g_now + 110000 + (guest_first ? 300 : 0);   /* the barrier opened at once; the movie runs ~110 s on both, ends 300 ms apart */
    G.placed_at = g_now + 110000 + (guest_first ? 0 : 300);
    G.fl[4374] = 1;                                             /* a stale flag of the guest's own save */
    run(&H, &G, &L1, &L2, 1, 125000);
    int bad = 0;
    bad += !(H.dig_built == 1 && !H.dig_before_place && H.dig_had_4449 == 1);   /* the digest describes the level after its start scripts */
    bad += G.prop4449 != 0;                                                     /* the guest's own level start never crosses as a proposal */
    bad += H.start_saw_set != 0;                                                /* the host's own start script ran on its own world */
    uint32_t first = 0;
    CHECK(H.started && G.started && H.fl[4449] == 1 && G.fl[4449] == 1);     /* whichever glue: both levels started */
    if (g_place_gate) {
        CHECK(same_flags(&H, &G, &first) && G.fl[4374] == 0);
        CHECK(G.s.digest_done && G.s.st.digest_rx == 1 && G.s.st.digest_timeout == 0);
        CHECK(w1_pending(&H.w) == 0 && w1_pending(&G.w) == 0);
    }
    return bad;
}

/* ================================================================ part D (ep1-probe): the pre-load digest
 * e1p1 live (SV probe): no restart deserializes the flag store; the title's save read loads it once. Model: the host Continues a save with progress flags
 * (its store = F), the guest's store holds its own title / old-level state G. The level start scripts of each PC read its store at its load (before the
 * post-load digest). With the pre-load digest the guest's store at its load equals the host's; the model before the fix (no pre-load digest) does not. */
static int D_h[65536], D_g[65536];
static int D_geth(void *u, uint32_t id) { (void)u; return id < 65536 ? D_h[id] : 0; }
static int D_getg(void *u, uint32_t id) { (void)u; return id < 65536 ? D_g[id] : 0; }
static int D_getneg(void *u, uint32_t id) { (void)u; return id == 4398 ? -1 : (id < 65536 ? D_g[id] : 0); }
static unsigned D_writes;
static void D_setg(void *u, uint32_t id, int32_t v) { (void)u; if (id < 65536) D_g[id] = v; D_writes++; }
static int D_same(void) { for (unsigned i = 0; i < SF1_N; i++) if ((D_h[sf1_id_at(i)] > 0) != (D_g[sf1_id_at(i)] > 0)) return 0; return 1; }
static void part_d(void)
{
    for (int model = 0; model < 2; model++) {   /* model 1 = before the fix: the guest's load reads its own store */
        memset(D_h, 0, sizeof D_h), memset(D_g, 0, sizeof D_g);
        int prog[] = {4382, 4384, 4385, 4388, 4390, 4398, 4399, 4432, 4449, 4457};   /* the host's save: 1-1 at CP1 */
        for (unsigned i = 0; i < sizeof prog / sizeof prog[0]; i++) D_h[prog[i]] = 1;
        D_g[4449] = 1, D_g[4864] = 1, D_g[4943] = 1;   /* the guest's own save: somewhere in 1-3 */
        sf1_pre_t pre;
        memset(&pre, 0, sizeof pre);
        uint8_t b[SF1_PRE_LEN + 4];
        if (model == 0) {
            unsigned n = sf1_pre_build(D_geth, NULL, 1, b, sizeof b);
            CHECK(n == SF1_PRE_LEN && SF1_PRE_LEN <= 96 && sf1_pre_take(&pre, b, n, 1000));
            unsigned s = 0, c = 0;
            D_writes = 0;
            CHECK(sf1_pre_apply(&pre, 2700, D_getg, D_setg, NULL, &s, &c) == 1 && s == 9 && c == 2 && D_writes == 11);
            CHECK(!pre.have && pre.applied == 1);
        }
        int same = D_same();
        if (model == 0) CHECK(same && D_g[9000] == 0);
        else {
            CHECK(!same);
            printf("  part D, the model before the fix: the guest's load reads its own store (equal to the host's: %d)\n", same);
        }
    }
    /* used once; a stale one is dropped; the newest seq wins; an older / the same seq does not replace; bad bodies refused; unknown ids left alone */
    sf1_pre_t p;
    memset(&p, 0, sizeof p);
    memset(D_h, 0, sizeof D_h), memset(D_g, 0, sizeof D_g);
    D_h[4398] = 1, D_h[4399] = 1;
    uint8_t b1[SF1_PRE_LEN], b2[SF1_PRE_LEN];
    CHECK(sf1_pre_build(D_geth, NULL, 5, b1, sizeof b1) == SF1_PRE_LEN && sf1_pre_build(D_geth, NULL, 5, b1, SF1_PRE_LEN - 1) == 0 && sf1_pre_build(NULL, NULL, 5, b1, sizeof b1) == 0);
    sf1_pre_build(D_geth, NULL, 5, b1, sizeof b1);
    D_h[4399] = 0;
    sf1_pre_build(D_geth, NULL, 6, b2, sizeof b2);
    CHECK(sf1_pre_take(&p, b1, sizeof b1, 100) && sf1_pre_take(&p, b2, sizeof b2, 200) && p.seq == 6);
    CHECK(!sf1_pre_take(&p, b1, sizeof b1, 300) && p.seq == 6 && !sf1_pre_take(&p, b2, sizeof b2, 300));
    CHECK(!sf1_pre_take(&p, b2, SF1_PRE_LEN - 1, 300) && p.bad == 1);
    uint8_t bb[SF1_PRE_LEN];
    memcpy(bb, b2, sizeof bb), bb[0] ^= 0xFF;
    CHECK(!sf1_pre_take(&p, bb, sizeof bb, 300) && p.bad == 2);
    unsigned s, c;
    D_g[4399] = 1;
    CHECK(sf1_pre_apply(&p, 400, D_getneg, D_setg, NULL, &s, &c) == 1 && s == 0 && c == 1 && D_g[4399] == 0 && D_g[4398] == 0);   /* 4398 unknown here: not written */
    CHECK(sf1_pre_apply(&p, 500, D_getg, D_setg, NULL, &s, &c) == 0 && s == 0 && c == 0);   /* used once */
    sf1_pre_build(D_geth, NULL, 7, b1, sizeof b1);
    CHECK(sf1_pre_take(&p, b1, sizeof b1, 1000) && sf1_pre_apply(&p, 1000 + SF1_PRE_MAX_MS + 1, D_getg, D_setg, NULL, &s, &c) == 0 && p.stale == 1 && !p.have);
}

int main(void)
{
    part_a();
    part_d();
    scenario(0, 0, 11);
    scenario(0, 20, 12);
    scenario(1, 0, 13);
    scenario(1, 20, 14);
    scenario(1, 35, 15);
    /* part C: the glue with the placement gate passes all; the glue before the fix fails (the movie case of e1w2) */
    for (int gf = 0; gf < 2; gf++) {
        g_place_gate = 1;
        int b0 = movie(gf, 0, 21 + (unsigned)gf), b1 = movie(gf, 20, 23 + (unsigned)gf);
        CHECK(b0 == 0 && b1 == 0);
        if (b0 || b1) printf("  (movie, guest first %d: %d / %d checks failed with the placement gate)\n", gf, b0, b1);
        g_place_gate = 0;
        int o = movie(gf, 0, 21 + (unsigned)gf);
        CHECK(o > 0);   /* the model before the fix must fail */
        printf("  part C, guest's movie ends first %d: the glue before the fix fails %d of 3 checks\n", gf, o);
    }
    g_place_gate = 1;
    printf("test_sf1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
