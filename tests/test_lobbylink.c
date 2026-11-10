/*
 * Offline tests of the network link of the Revelations 1 lobby (native/lobby_link.c) on the in-process "sim" transport, driven through
 * the lobby model the way menu.c drives it: Start / Disconnect rows -> L1R_NET_START / L1R_NET_STOP -> lk_start / lk_stop, lk_poll once
 * a frame. Checks the link state the model holds, the status line the player reads, the two ready flags (both directions, and that a
 * ready flag does not survive a link that went away), host loss, reconnect, failed starts. Real time (the sim delivers at once), a few seconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_net.h"
#include "../native/lobby1.h"
#include "../native/lobby_link.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static l1_t *open_lobby(int mode, const char *join_addr)
{
    l1_t *l = l1_new();
    l1_settings s;
    memset(&s, 0, sizeof s);
    s.armed = 1, s.net_mode = mode;
    snprintf(s.join_addr, sizeof s.join_addr, "%s", join_addr ? join_addr : "");
    l1_env env;
    memset(&env, 0, sizeof env);
    env.lang = 0, env.net_rows = 1;
    l1_open(l, &s, &env);
    return l;
}

/* the status line the player reads, compared with its text */
static int status_is(l1_t *l, int text_id)
{
    l1_view v;
    l1_view_get(l, &v);
    return strcmp(v.net_status, l1_text(text_id, 0)) == 0;
}

/* the status line contains this text (the network lines are composed: "<name> has joined.  Mark yourself ready ...") */
static int status_has(l1_t *l, const char *text)
{
    l1_view v;
    l1_view_get(l, &v);
    return strstr(v.net_status, text) != NULL;
}
static int status_eq(l1_t *l, const char *text)
{
    l1_view v;
    l1_view_get(l, &v);
    if (strcmp(v.net_status, text)) printf("  status is \"%s\", expected \"%s\"\n", v.net_status, text);
    return strcmp(v.net_status, text) == 0;
}

static int status_kind(l1_t *l)
{
    l1_view v;
    l1_view_get(l, &v);
    return v.net_status_kind;
}

/* index of the row with this label (-1 when the row is not shown now) */
static int find_row(l1_t *l, int label_id)
{
    l1_view v;
    l1_view_get(l, &v);
    for (int i = 0; i < v.nrows; i++)
        if (!strcmp(v.rows[i].label, l1_text(label_id, 0))) return i;
    return -1;
}

static void click(l1_t *l, int label_id)
{
    int row = find_row(l, label_id);
    l1_event e;
    memset(&e, 0, sizeof e);
    e.kind = L1E_CLICK, e.row = row;
    if (row >= 0) l1_event_in(l, &e);
}

/* the glue's part of the request: a Start / Disconnect of the model becomes a link start / stop */
static int take_and_run(l1_t *l, lk_t *k, char *own, size_t osz)
{
    int req = l1_take_request(l);
    l1_settings s;
    l1_settings_get(l, &s);
    char err[160];
    if (req == L1R_NET_START) return lk_start(k, s.net_mode, s.join_addr, LK_DEFAULT_PORT, own, osz, err, sizeof err);
    if (req == L1R_NET_STOP) { lk_stop(k); return 2; }
    return -1;
}

static void pump(lk_t *a, lk_t *b, int ms)
{
    for (int t = 0; t < ms; t += 2) {
        if (a) lk_poll(a);
        if (b) lk_poll(b);
        Sleep(2);
    }
}

static int wait_until(lk_t *a, lk_t *b, int ms, int (*done)(void *), void *u)
{
    for (int t = 0; t < ms; t += 2) {
        if (a) lk_poll(a);
        if (b) lk_poll(b);
        if (done(u)) return 1;
        Sleep(2);
    }
    return done(u);
}

typedef struct { l1_t *l; int state; } Want;
static int state_is(void *u)
{
    Want *w = (Want *)u;
    return l1_net_state(w->l) == w->state;
}

static int both_connected(void *u)
{
    l1_t **p = (l1_t **)u;
    return l1_net_state(p[0]) == L1NS_CONNECTED && l1_net_state(p[1]) == L1NS_CONNECTED;
}

int main(void)
{
    cn_sim_real_time();
    char own[64], err[160];

    /* ---- 1. host: Start from the model listens; the status line says so; the own address is shown in the model */
    {
        l1_t *h = open_lobby(L1N_HOST, "");
        lk_t *hk = lk_new(h, "sim", 0);
        CHECK(hk != NULL);
        CHECK(find_row(h, L1T_NET_START) >= 0);
        click(h, L1T_NET_START);
        CHECK(take_and_run(h, hk, own, sizeof own) == 1);
        CHECK(l1_net_state(h) == L1NS_WAIT);
        CHECK(status_is(h, L1T_NET_ST_WAIT));
        CHECK(lk_active(hk));
        CHECK(find_row(h, L1T_NET_STOP) >= 0);   /* the row became Disconnect */
        lk_stop(hk);
        CHECK(l1_net_state(h) == L1NS_IDLE);
        CHECK(!lk_active(hk));
        lk_free(hk);
        l1_free(h);
    }

    /* ---- 2. a full pair: join connects (CONNECTING -> CONNECTED on both), ready in both directions, stop, reconnect, host loss */
    {
        /* the host listens on the default port (sim 28410); the joiner types "sim:28410" */
        l1_t *h = open_lobby(L1N_HOST, "");
        l1_t *c = open_lobby(L1N_JOIN, "sim:28410");
        lk_config manual;   /* this test is about the link; the automatic start of the session (both ready) is test_netcore's */
        memset(&manual, 0, sizeof manual);
        manual.manual_start = 1;
        lk_t *hk = lk_new_cfg(h, "sim", 0, &manual);
        lk_t *ck = lk_new_cfg(c, "sim", 0, &manual);

        /* the host starts first */
        click(h, L1T_NET_START);
        CHECK(take_and_run(h, hk, own, sizeof own) == 1);
        CHECK(l1_net_state(h) == L1NS_WAIT);

        /* the joiner starts: connecting, then connected on both sides */
        click(c, L1T_NET_START);
        CHECK(take_and_run(c, ck, err, sizeof err) == 1);
        CHECK(l1_net_state(c) == L1NS_CONNECTING);
        CHECK(status_eq(c, "Connecting to sim:28410..."));
        {
            l1_t *pair[2] = {h, c};
            CHECK(wait_until(hk, ck, 3000, both_connected, pair));
        }
        CHECK(status_eq(h, "Player has joined.  Mark yourself ready when you are set."));   /* the partner's name (lk_new's default name) */
        CHECK(status_eq(c, "Joining as Parker (partner).  Mark yourself ready when you are set."));   /* the guest is told what the host gave him */
        CHECK(status_kind(c) == L1S_INFO);

        /* the ready row is shown and selectable only when connected: the joiner readies, the host sees it, the joiner waits */
        CHECK(find_row(c, L1T_NET_READY) >= 0);
        click(c, L1T_NET_READY);
        CHECK(l1_net_ready(c) == 1);
        pump(hk, ck, 60);
        CHECK(status_has(h, "Player is ready."));
        CHECK(status_has(c, l1_text(L1T_NET_ST_WAITREADY, 0)));

        /* the host readies too: both ready, shown as OK on both sides */
        click(h, L1T_NET_READY);
        CHECK(l1_net_ready(h) == 1);
        pump(hk, ck, 60);
        CHECK(status_has(h, l1_text(L1T_NET_ST_BOTH, 0)) && status_kind(h) == L1S_OK);
        CHECK(status_has(c, l1_text(L1T_NET_ST_BOTH, 0)) && status_kind(c) == L1S_OK);

        /* the joiner takes the ready back (the host is still ready): the host waits for him, the joiner sees the host ready */
        click(c, L1T_NET_READY);
        CHECK(l1_net_ready(c) == 0);
        pump(hk, ck, 60);
        CHECK(status_has(h, l1_text(L1T_NET_ST_WAITREADY, 0)));
        CHECK(status_has(c, "Player is ready."));

        /* the joiner readies again: both ready, then he disconnects: the host goes back to waiting and drops its own ready flag */
        click(c, L1T_NET_READY);
        pump(hk, ck, 60);
        CHECK(status_has(h, l1_text(L1T_NET_ST_BOTH, 0)));
        click(c, L1T_NET_STOP);   /* the joiner's Disconnect row */
        CHECK(take_and_run(c, ck, err, sizeof err) == 2);
        CHECK(l1_net_state(c) == L1NS_IDLE);
        CHECK(l1_net_ready(c) == 0);
        {
            Want w = {h, L1NS_WAIT};
            CHECK(wait_until(hk, NULL, 3000, state_is, &w));
        }
        CHECK(status_eq(h, "Player has left.  Waiting for the other player to join..."));   /* the host is told who left, and keeps listening */
        CHECK(status_kind(h) == L1S_WARN && l1_net_reason(h) == L1RS_PARTNER_LEFT);

        /* reconnect: the host's ready flag was cleared with the link, the new joiner must not see a stale "ready" */
        CHECK(l1_net_ready(h) == 0);
        click(c, L1T_NET_START);
        CHECK(take_and_run(c, ck, err, sizeof err) == 1);
        {
            l1_t *pair[2] = {h, c};
            CHECK(wait_until(hk, ck, 3000, both_connected, pair));
        }
        pump(hk, ck, 40);
        CHECK(status_eq(h, "Player has joined.  Mark yourself ready when you are set."));   /* the old reason went with the new partner */
        CHECK(status_eq(c, "Joining as Parker (partner).  Mark yourself ready when you are set."));

        /* the host leaves (Disconnect on the host): the joiner is told that the host ended the session (not a generic "connection failed") */
        click(h, L1T_NET_STOP);
        CHECK(take_and_run(h, hk, own, sizeof own) == 2);
        CHECK(l1_net_state(h) == L1NS_IDLE);
        {
            Want w = {c, L1NS_ERROR};
            CHECK(wait_until(NULL, ck, 3000, state_is, &w));
        }
        CHECK(status_eq(c, "The host has ended the session.") && status_kind(c) == L1S_WARN && l1_net_reason(c) == L1RS_HOST_ENDED);
        CHECK(lk_last_reason(ck)[0] != 0);
        /* after the error the joiner can try again: Start is shown again (the link is not active) */
        CHECK(!lk_active(ck));
        CHECK(find_row(c, L1T_NET_START) >= 0);
        lk_stop(ck);
        CHECK(l1_net_state(c) == L1NS_IDLE);

        lk_free(hk);
        lk_free(ck);
        l1_free(h);
        l1_free(c);
    }

    /* ---- 3. failed starts: a join with no host on that address is an error at once; a second host on the same port too */
    {
        l1_t *c = open_lobby(L1N_JOIN, "sim:99");
        lk_t *ck = lk_new(c, "sim", 0);
        click(c, L1T_NET_START);
        CHECK(take_and_run(c, ck, err, sizeof err) == 0);
        CHECK(l1_net_state(c) == L1NS_ERROR);
        CHECK(status_is(c, L1T_NET_ST_ERROR));
        CHECK(!lk_active(ck));
        lk_stop(ck);
        CHECK(l1_net_state(c) == L1NS_IDLE);
        lk_free(ck);
        l1_free(c);

        l1_t *h1 = open_lobby(L1N_HOST, "");
        l1_t *h2 = open_lobby(L1N_HOST, "");
        lk_t *k1 = lk_new(h1, "sim", 0);
        lk_t *k2 = lk_new(h2, "sim", 0);
        char own2[64];
        CHECK(lk_start(k1, L1N_HOST, "", 9, own2, sizeof own2, err, sizeof err) == 1);
        CHECK(lk_start(k2, L1N_HOST, "", 9, own2, sizeof own2, err, sizeof err) == 0);   /* port 9 is taken by k1 */
        CHECK(l1_net_state(h2) == L1NS_ERROR);
        CHECK(strstr(lk_last_reason(k2), "in use") != NULL);
        lk_stop(k1);
        lk_stop(k2);
        lk_free(k1);
        lk_free(k2);
        l1_free(h1);
        l1_free(h2);
    }

    /* ---- 4. the ready row does nothing while the link is not up (it is grey) */
    {
        l1_t *c = open_lobby(L1N_JOIN, "sim:28410");
        CHECK(find_row(c, L1T_NET_READY) >= 0);
        click(c, L1T_NET_READY);
        CHECK(l1_net_ready(c) == 0);
        l1_free(c);
    }

    /* ---- 5. lobby-must: Remove partner (kick) from the host's lobby; the guest reads the game's own "removed" text; the host listens again */
    {
        l1_t *h = open_lobby(L1N_HOST, "");
        l1_t *c = open_lobby(L1N_JOIN, "sim:28411");
        lk_config hc, cc;
        memset(&hc, 0, sizeof hc), memset(&cc, 0, sizeof cc);
        hc.manual_start = cc.manual_start = 1, hc.name = "Hosty", cc.name = "Guesty";
        lk_t *hk = lk_new_cfg(h, "sim", 0, &hc);
        lk_t *ck = lk_new_cfg(c, "sim", 0, &cc);
        CHECK(lk_start(hk, L1N_HOST, "", 28411, own, sizeof own, err, sizeof err) == 1);
        click(c, L1T_NET_START);
        CHECK(take_and_run(c, ck, err, sizeof err) == 1);
        {
            l1_t *pair[2] = {h, c};
            CHECK(wait_until(hk, ck, 3000, both_connected, pair));
        }
        pump(hk, ck, 40);
        CHECK(status_eq(h, "Guesty has joined.  Mark yourself ready when you are set."));
        CHECK(find_row(h, L1T_KICK) >= 0 && find_row(c, L1T_KICK) < 0);   /* only the host can remove */
        click(h, L1T_KICK);
        CHECK(l1_take_request(h) == L1R_NONE);   /* the first press asks */
        {
            l1_view v;
            l1_view_get(h, &v);
            int r = -1;
            for (int i = 0; i < v.nrows; i++)
                if (strstr(v.rows[i].label, "kick Guesty")) r = i;
            CHECK(r >= 0);
            l1_event e = {L1E_CLICK, r, 0};
            l1_event_in(h, &e);
        }
        CHECK(l1_take_request(h) == L1R_NET_KICK);
        CHECK(lk_kick(hk) == 1);
        {
            Want w = {c, L1NS_ERROR};
            CHECK(wait_until(hk, ck, 3000, state_is, &w));
        }
        pump(hk, ck, 40);
        CHECK(status_eq(c, "You have been removed from the Lobby by the host.") && l1_net_reason(c) == L1RS_REMOVED);
        CHECK(l1_net_state(h) == L1NS_WAIT && l1_net_reason(h) == L1RS_NONE && status_eq(h, "Waiting for the other player to join..."));
        lk_info ci;
        lk_get_info(ck, &ci);
        CHECK(ci.ended == 1 && ci.ended_reason == L1RS_REMOVED && ci.reason == L1RS_REMOVED);
        CHECK(lk_kick(hk) == 0);   /* nobody left to remove */
        lk_stop(hk), lk_stop(ck);
        lk_free(hk), lk_free(ck);
        l1_free(h), l1_free(c);
    }

    /* ---- 6. lobby-must: another mod version: the guest reads both versions in its language; the host's name travels */
    {
        l1_t *c = open_lobby(L1N_JOIN, "sim:28412");
        lk_config hc, cc;
        memset(&hc, 0, sizeof hc), memset(&cc, 0, sizeof cc);
        hc.mod_version = "0.1.1", cc.mod_version = "0.1.0", hc.name = "Hosty";
        lk_t *hk = lk_new_cfg(NULL, "sim", 0, &hc);
        lk_t *ck = lk_new_cfg(c, "sim", 0, &cc);
        CHECK(lk_start(hk, L1N_HOST, "", 28412, own, sizeof own, err, sizeof err) == 1);
        click(c, L1T_NET_START);
        CHECK(take_and_run(c, ck, err, sizeof err) == 1);
        {
            Want w = {c, L1NS_ERROR};
            CHECK(wait_until(hk, ck, 3000, state_is, &w));
        }
        CHECK(status_eq(c, "Co-op mod versions differ: host 0.1.1, you 0.1.0. Start the game from the Co-op shortcut to update it.") && l1_net_reason(c) == L1RS_MODVER);
        lk_stop(hk), lk_stop(ck);
        lk_free(hk), lk_free(ck);
        l1_free(c);
    }

    /* ---- 7. lobby-must: the host ends the session after the START: the guest's link counts the end (the in-game strip) and names the reason */
    {
        l1_t *h = open_lobby(L1N_HOST, "");
        l1_t *c = open_lobby(L1N_JOIN, "sim:28413");
        lk_config hc, cc;
        memset(&hc, 0, sizeof hc), memset(&cc, 0, sizeof cc);
        hc.name = "Hosty", cc.name = "Guesty";
        lk_t *hk = lk_new_cfg(h, "sim", 0, &hc);
        lk_t *ck = lk_new_cfg(c, "sim", 0, &cc);
        CHECK(lk_start(hk, L1N_HOST, "", 28413, own, sizeof own, err, sizeof err) == 1);
        click(c, L1T_NET_START);
        CHECK(take_and_run(c, ck, err, sizeof err) == 1);
        {
            l1_t *pair[2] = {h, c};
            CHECK(wait_until(hk, ck, 3000, both_connected, pair));
        }
        l1_net_set_ready(h, 1), l1_net_set_ready(c, 1);   /* both ready: the host starts by itself */
        pump(hk, ck, 200);
        lk_info hi, ci;
        lk_get_info(hk, &hi), lk_get_info(ck, &ci);
        CHECK(hi.started && ci.started);
        CHECK(status_eq(h, "Start Episode 1: Campaign > Continue or New Game. Guesty follows you automatically."));
        CHECK(status_eq(c, "Waiting for Hosty to start the story. Your game follows automatically."));
        lk_stop(hk);
        {
            Want w = {c, L1NS_ERROR};
            CHECK(wait_until(NULL, ck, 3000, state_is, &w));
        }
        lk_get_info(ck, &ci);
        CHECK(ci.ended == 1 && ci.ended_reason == L1RS_HOST_ENDED && !strcmp(ci.last_peer, "Hosty"));
        lk_stop(ck);
        lk_free(hk), lk_free(ck);
        l1_free(h), l1_free(c);
    }

    /* ---- 8. lm1 live: a join started from the console and a lobby opened later shows the typed address (it showed "type IP:port"), the selection is on a usable row */
    {
        lk_config hc;
        memset(&hc, 0, sizeof hc);
        lk_t *hk = lk_new_cfg(NULL, "sim", 0, &hc);
        lk_t *ck = lk_new_cfg(NULL, "sim", 0, &hc);
        CHECK(lk_start(hk, L1N_HOST, "", 28414, own, sizeof own, err, sizeof err) == 1);
        CHECK(lk_start(ck, L1N_JOIN, "sim:28414", 0, NULL, 0, err, sizeof err) == 1);
        Want w = {NULL, 0};
        (void)w;
        pump(hk, ck, 200);
        l1_t *c = open_lobby(L1N_OFF, "");
        lk_set_lobby(ck, c);
        l1_view v;
        l1_view_get(c, &v);
        int ar = find_row(c, L1T_NET_ADDR);
        CHECK(ar >= 0 && !strcmp(v.rows[ar].value, "sim:28414"));
        CHECK(v.rows[0].kind == L1V_GREY && v.sel != 0);   /* the Mode row is locked while the link runs: the selection moved on */
        lk_stop(hk), lk_stop(ck);
        lk_free(hk), lk_free(ck);
        l1_free(c);
    }

    printf("test_lobbylink: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
