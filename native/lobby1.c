/*
 * lobby1.c - model of the Revelations 1 co-op lobby (see lobby1.h). Pure C: no Windows, no game memory.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lobby1.h"

enum { A_NONE = 0, A_ARMED, A_ROLE, A_P2, A_LAYOUT, A_PADS, A_PADINFO, A_DONE, A_NET_MODE, A_NET_ADDR, A_NET_GO, A_NET_READY, A_MODE, A_KICK, A_NET_WAY, A_NET_INVITE, A_DOWNED, A_NET_FRIEND };

struct l1 {
    l1_settings s;
    l1_env env;
    int sel;
    int request;
    int changed;
    int open;
    int net_state;      /* L1NS_*: the link, reported by the network glue */
    int ready;          /* this player marked "I am ready" (only while connected) */
    int peer_ready;     /* the other player is ready */
    int net_role;       /* the role the host gave this player (L1_LEAD / L1_PARTNER), -1 = none; shown instead of settings.role while a Join link is CONNECTED */
    int reason;         /* L1RS_*: why the link failed / what happened to the partner (shown until the next start) */
    char ra1[64], ra2[24];
    char peer[40];      /* the other player's name */
    int started;        /* the session START came */
    int kick_ask;       /* the Remove partner row asks for its confirmation */
    int host_flags;     /* ep1-flow E12, a guest: the host's session flags byte (-1 = not known) */
    int friend_sel;     /* rel010: the friend the friend row shows (index into env.friend_*) */
    int evt_ok;         /* rel010: the change() in progress comes from OK / click (confirm), not from left / right */
};

/* ---------------------------------------------------------------- person names */
static const char *const k_person_en[7] = {"Jill", "Chris", "Parker", "Keith", "Jessica", "Quint", "Raymond"};
static char g_stock_names[7][48];

/* first name of a full name: up to the first space, middle dot or katakana middle dot */
static void first_name(const char *full, char *out, size_t size)
{
    size_t n = 0;
    for (const char *p = full; *p && n + 1 < size; p++) {
        if (*p == ' ') break;
        if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB7) break;                                           /* U+00B7 */
        if ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x83 && (unsigned char)p[2] == 0xBB) break;            /* U+30FB */
        out[n++] = *p;
    }
    out[n] = 0;
}

void l1_set_stock_names(const char names[7][48])
{
    for (int i = 0; i < 7; i++) {
        g_stock_names[i][0] = 0;
        if (names && names[i][0]) first_name(names[i], g_stock_names[i], sizeof g_stock_names[i]);
    }
}

const char *l1_person_name(int person, int lang)
{
    (void)lang;
    if (person < 0 || person > 6) return "?";
    return g_stock_names[person][0] ? g_stock_names[person] : k_person_en[person];
}

static int ci_contains(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

int l1_person_from_class(const char *cls)
{
    if (!cls) return -1;
    static const struct { const char *key; int person; } map[] = {{"Jill", 0}, {"Chris", 1}, {"Parker", 2}, {"Keith", 3}, {"Jessica", 4}, {"Quint", 5}, {"Reymond", 6}, {"Raymond", 6}};
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (ci_contains(cls, map[i].key)) return map[i].person;
    return -1;
}

/* ---------------------------------------------------------------- assignment */
void l1_assign_get(const l1_settings *s, l1_assign *a)
{
    a->unit1 = s->role == L1_PARTNER ? 1 : 0;
    a->unit2 = 1 - a->unit1;
    a->pad1 = 0;
    a->pad2 = 1;
    a->swap = s->pads_swapped ? 1 : 0;
}

/* ---------------------------------------------------------------- model */
l1_t *l1_new(void)
{
    l1_t *l = calloc(1, sizeof *l);
    if (l) {
        l->s.role = L1_LEAD;
        l->s.layout = L1_LAYOUT_SIDE;
        l->net_role = -1;
        l->host_flags = -1;
    }
    return l;
}
void l1_free(l1_t *l) { free(l); }

int l1_mode_of(const l1_settings *s)
{
    if (s->net_mode == L1N_HOST) return L1M_HOST;
    if (s->net_mode == L1N_JOIN) return L1M_JOIN;
    return s->armed ? L1M_SPLIT : L1M_OFF;
}

void l1_mode_set(l1_settings *s, int mode)
{
    s->armed = mode == L1M_SPLIT;
    s->net_mode = mode == L1M_HOST ? L1N_HOST : mode == L1M_JOIN ? L1N_JOIN : L1N_OFF;
}

static void sanitize(l1_settings *s)
{
    s->armed = s->armed != 0;
    s->role = s->role == L1_PARTNER ? L1_PARTNER : L1_LEAD;
    s->layout = s->layout == L1_LAYOUT_TOPBOTTOM ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE;
    s->pads_swapped = s->pads_swapped != 0;
    s->net_mode = s->net_mode == L1N_HOST || s->net_mode == L1N_JOIN ? s->net_mode : L1N_OFF;
    if (s->net_mode != L1N_OFF) s->armed = 0;   /* one mode: one human per PC in a network game, never the local split as well */
    s->join_addr[sizeof s->join_addr - 1] = 0;
    s->way = s->way != 0;
    if (s->downed < 0 || s->downed >= L1D_N) s->downed = L1D_60;
}

/* "host[:port]": host = letters, digits, '.', '-'; port 1..65535 when given */
int l1_addr_ok(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s), i = 0, hs = 0;
    while (i < n && s[i] != ':') {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-')) return 0;
        i++, hs++;
    }
    if (!hs || hs > 40) return 0;
    if (i == n) return 1;
    i++;   /* the ':' */
    if (i == n) return 0;
    long port = 0;
    for (; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
        port = port * 10 + (s[i] - '0');
        if (port > 65535) return 0;
    }
    return port >= 1;
}

void l1_net_set_state(l1_t *l, int state)
{
    if (state < L1NS_IDLE || state > L1NS_ERROR) state = L1NS_IDLE;
    if (state != L1NS_CONNECTED) l->ready = 0, l->peer_ready = 0, l->net_role = -1, l->started = 0, l->kick_ask = 0;   /* ready means "in this link": a new link starts unready */
    if ((state == L1NS_WAIT || state == L1NS_CONNECTING) && (l->net_state == L1NS_IDLE || l->net_state == L1NS_ERROR)) l->reason = L1RS_NONE, l->ra1[0] = l->ra2[0] = 0;   /* a new attempt */
    if (state == L1NS_IDLE) l->reason = L1RS_NONE, l->ra1[0] = l->ra2[0] = 0, l->peer[0] = 0;
    if (state == L1NS_WAIT || state == L1NS_CONNECTING) l->peer[0] = 0;
    l->net_state = state;
}

void l1_net_set_reason(l1_t *l, int reason, const char *a1, const char *a2)
{
    l->reason = reason > L1RS_NONE && reason < L1RS_COUNT ? reason : L1RS_NONE;
    snprintf(l->ra1, sizeof l->ra1, "%s", a1 ? a1 : "");
    snprintf(l->ra2, sizeof l->ra2, "%s", a2 ? a2 : "");
}
int l1_net_reason(const l1_t *l) { return l->reason; }
void l1_net_set_peer(l1_t *l, const char *name) { snprintf(l->peer, sizeof l->peer, "%s", name ? name : ""); }
void l1_net_set_started(l1_t *l, int started) { l->started = started != 0 && l->net_state == L1NS_CONNECTED; if (l->started) l->kick_ask = 0; }
void l1_net_set_host_flags(l1_t *l, int flags) { l->host_flags = flags >= 0 ? flags & 0xFF : -1; }

/* ---------------------------------------------------------------- ep1-flow E12: the Downed / revive rule */
static const int k_downed_order[L1D_N] = {L1D_OFF, L1D_30, L1D_60, L1D_120, L1D_NEVER};   /* the row cycles in this order */
unsigned l1_downed_flags(int d)
{
    switch (d) {   /* bit 0 = the rule, bits 1..2 = the bleed-out preset (downed_logic.h dl_preset_ms: 0 60 s, 1 30 s, 2 120 s, 3 never) */
    case L1D_OFF: return 0u;
    case L1D_30: return 1u | (1u << 1);
    case L1D_120: return 1u | (2u << 1);
    case L1D_NEVER: return 1u | (3u << 1);
    default: return 1u;   /* L1D_60 and anything unknown: the default */
    }
}
int l1_downed_from_flags(unsigned f)
{
    if (!(f & 1u)) return L1D_OFF;
    switch ((f >> 1) & 3u) {
    case 1: return L1D_30;
    case 2: return L1D_120;
    case 3: return L1D_NEVER;
    default: return L1D_60;
    }
}
void l1_downed_text(int d, int lang, char *out, size_t size)
{
    if (!size) return;
    if (d == L1D_OFF) snprintf(out, size, "%s", l1_stock_have(L1K_OFF) ? l1_stock(L1K_OFF) : l1_text(L1T_OFF, lang));   /* stock first: the options menu's own word */
    else if (d == L1D_NEVER) snprintf(out, size, "%s", l1_text(L1T_NO_LIMIT, lang));
    else l1_fmt(out, size, l1_text(L1T_SECS, lang), d == L1D_30 ? "30" : d == L1D_120 ? "120" : "60", NULL);
}
static int downed_step(int d, int dir)
{
    int i = 0;
    while (i < L1D_N && k_downed_order[i] != d) i++;
    if (i == L1D_N) i = 2;   /* unknown: from the default */
    return k_downed_order[(i + dir + L1D_N) % L1D_N];
}

static int role_locked(const l1_t *l) { return l->net_role >= 0 && l->net_state == L1NS_CONNECTED && l->s.net_mode == L1N_JOIN; }
/* the role this player plays: a joined player plays what the host gave him; a host / a local player plays what he chose */
static int eff_role(const l1_t *l)
{
    return role_locked(l) ? l->net_role : l->s.role;
}

void l1_net_set_assigned(l1_t *l, int role) { l->net_role = role == L1_LEAD || role == L1_PARTNER ? role : -1; }
void l1_net_set_ready(l1_t *l, int ready) { if (l->net_state == L1NS_CONNECTED) l->ready = ready != 0; }
void l1_net_set_role(l1_t *l, int role) { l->s.role = role == L1_PARTNER ? L1_PARTNER : L1_LEAD; }
void l1_net_adopt(l1_t *l, int mode, const char *addr)
{
    l->s.net_mode = mode == L1N_HOST || mode == L1N_JOIN ? mode : L1N_OFF;
    if (l->s.net_mode != L1N_OFF) l->s.armed = 0;
    if (addr && mode == L1N_JOIN) snprintf(l->s.join_addr, sizeof l->s.join_addr, "%s", addr);
}

void l1_net_set_peer_ready(l1_t *l, int ready) { l->peer_ready = ready != 0; }
int l1_net_ready(const l1_t *l) { return l->ready; }
int l1_net_state(const l1_t *l) { return l->net_state; }

/* the link is up or being set up: the mode and the address cannot be changed now */
static int link_active(const l1_t *l) { return l->net_state == L1NS_WAIT || l->net_state == L1NS_CONNECTING || l->net_state == L1NS_CONNECTED; }

void l1_open(l1_t *l, const l1_settings *s, const l1_env *env)
{
    if (s) l->s = *s;
    sanitize(&l->s);
    if (env) l->env = *env;
    if (l->env.lang < 0 || l->env.lang >= L1_LANGS) l->env.lang = 0;
    l->sel = 0;
    l->request = L1R_NONE;
    l->changed = 0;
    l->open = 1;
    l->kick_ask = 0;
}

void l1_set_env(l1_t *l, const l1_env *env)
{
    if (!env) return;
    l->env = *env;
    if (l->env.lang < 0 || l->env.lang >= L1_LANGS) l->env.lang = 0;
}

static const char *lead_name(const l1_t *l) { return l->env.lead[0] ? l->env.lead : l1_person_name(0, l->env.lang); }
static const char *partner_name(const l1_t *l) { return l->env.partner[0] ? l->env.partner : l1_person_name(2, l->env.lang); }

/* "Jill (lead character)" for a role */
static void who_text(const l1_t *l, int role, char *out, size_t size)
{
    snprintf(out, size, "%s (%s)", role == L1_PARTNER ? partner_name(l) : lead_name(l), l1_text(role == L1_PARTNER ? L1T_PARTNER : L1T_LEAD, l->env.lang));
}

/* the other player's name for the texts: his own (the Steam persona he sends), else "PLAYER 1" / "PLAYER 2" by the side he is on */
static const char *peer_word(const l1_t *l)
{
    if (l->peer[0]) return l->peer;
    return l1_text(l->s.net_mode == L1N_HOST ? L1T_PLAYER2 : L1T_PLAYER1, l->env.lang);
}

#define ROW(act, knd, lab, val) do { snprintf(rows[n].label, sizeof rows[n].label, "%s", (lab)); snprintf(rows[n].value, sizeof rows[n].value, "%s", (val)); rows[n].kind = (knd); rows[n].action = (act); n++; } while (0)

/* the local screen (env.net_rows = 0, REV1_NETROWS=0): the split-screen settings page exactly as before the network */
static int build_rows_local(const l1_t *l, l1_row *rows)
{
    int lang = l->env.lang, n = 0;
    char who1[120], who2[120], v[160];
    int role = l->s.role;
    who_text(l, role, who1, sizeof who1);
    who_text(l, 1 - role, who2, sizeof who2);
    snprintf(v, sizeof v, "< %s >", l1_text(l->s.armed ? L1T_ON : L1T_OFF, lang));
    ROW(A_ARMED, L1V_NORMAL, l1_text(L1T_COOP, lang), v);
    int g = l->s.armed ? L1V_NORMAL : L1V_GREY;
    snprintf(v, sizeof v, "< %s >", who1);
    ROW(A_ROLE, g, l1_text(L1T_P1PLAYS, lang), v);
    ROW(A_P2, l->s.armed ? L1V_INFO : L1V_GREY, l1_text(L1T_P2PLAYS, lang), who2);
    snprintf(v, sizeof v, "< %s >", l1_text(l->s.layout == L1_LAYOUT_TOPBOTTOM ? L1T_TOPBOTTOM : L1T_SIDE, lang));
    ROW(A_LAYOUT, g, l1_text(L1T_LAYOUT, lang), v);
    snprintf(v, sizeof v, "< %s >", l1_text(l->s.pads_swapped ? L1T_PADS_SWAPPED : L1T_PADS_NORMAL, lang));
    ROW(A_PADS, g, l1_text(L1T_PADS, lang), v);
    snprintf(v, sizeof v, "%d", l->env.pads_connected);
    ROW(A_PADINFO, l->s.armed && l->env.pads_connected < 2 ? L1V_NORMAL : L1V_INFO, l1_text(L1T_PADS_FOUND, lang), v);
    ROW(A_DONE, L1V_NORMAL, l1_text(L1T_DONE, lang), "");
    return n;
}

static const int k_mode_word[4] = {L1T_OFF, L1T_MODE_SPLIT, L1T_MODE_HOST, L1T_MODE_JOIN};

/* the product screen: one exclusive Mode row (Off / Split screen / Network: host / Network: join), then only the rows of that mode */
static int build_rows(const l1_t *l, l1_row *rows)
{
    if (!l->env.net_rows) return build_rows_local(l, rows);
    int lang = l->env.lang, n = 0;
    int mode = l1_mode_of(&l->s), busy = link_active(l), connected = l->net_state == L1NS_CONNECTED, started = l->started && connected;
    char who1[120], who2[120], v[200];
    int role = eff_role(l);
    who_text(l, role, who1, sizeof who1);
    who_text(l, 1 - role, who2, sizeof who2);
    snprintf(v, sizeof v, "< %s >", l1_text(k_mode_word[mode], lang));
    ROW(busy ? A_NONE : A_MODE, busy ? L1V_GREY : L1V_NORMAL, l1_text(L1T_COOP, lang), v);   /* a running link keeps its mode */
    if (mode == L1M_SPLIT) {
        snprintf(v, sizeof v, "< %s >", who1);
        ROW(A_ROLE, L1V_NORMAL, l1_text(L1T_P1PLAYS, lang), v);
        ROW(A_P2, L1V_INFO, l1_text(L1T_P2PLAYS, lang), who2);
        snprintf(v, sizeof v, "< %s >", l1_text(l->s.layout == L1_LAYOUT_TOPBOTTOM ? L1T_TOPBOTTOM : L1T_SIDE, lang));
        ROW(A_LAYOUT, L1V_NORMAL, l1_text(L1T_LAYOUT, lang), v);
        snprintf(v, sizeof v, "< %s >", l1_text(l->s.pads_swapped ? L1T_PADS_SWAPPED : L1T_PADS_NORMAL, lang));
        ROW(A_PADS, L1V_NORMAL, l1_text(L1T_PADS, lang), v);
        snprintf(v, sizeof v, "%d", l->env.pads_connected);
        ROW(A_PADINFO, l->env.pads_connected < 2 ? L1V_NORMAL : L1V_INFO, l1_text(L1T_PADS_FOUND, lang), v);
    } else if (mode == L1M_HOST) {
        snprintf(v, sizeof v, "< %s >", who1);
        ROW(started ? A_NONE : A_ROLE, started ? L1V_GREY : L1V_NORMAL, l1_text(L1T_YOU_PLAY, lang), v);   /* the host picks; after the start the roles are final */
        if (l->s.way) ROW(A_NONE, L1V_INFO, l1_text(L1T_STEAM_HINT, lang), "");   /* friends join from their Steam friends list; no address, no port */
        else ROW(A_NONE, L1V_INFO, l1_text(L1T_NET_OWN, lang), l->env.own_addr[0] ? l->env.own_addr : "?");
        ROW(A_NET_GO, L1V_NORMAL, l1_text(busy ? L1T_NET_STOP : L1T_NET_START, lang), "");
        snprintf(v, sizeof v, "< %s >", l1_text(l->ready ? L1T_ON : L1T_OFF, lang));
        ROW(connected && !started ? A_NET_READY : A_NONE, connected && !started ? L1V_NORMAL : L1V_GREY, l1_text(L1T_NET_READY, lang), v);
        if (connected && !started) {   /* Remove partner: a second OK confirms (the game's own question) */
            if (l->kick_ask) {
                char q[160];
                l1_fmt(q, sizeof q, l1_stock(L1K_KICK_ASK), peer_word(l), NULL);
                ROW(A_KICK, L1V_NORMAL, q, l1_text(L1T_LEG_OK, lang));
            } else
                ROW(A_KICK, L1V_NORMAL, l1_text(L1T_KICK, lang), "");
        }
    } else if (mode == L1M_JOIN) {
        ROW(busy ? A_NONE : A_NET_ADDR, busy ? L1V_GREY : L1V_NORMAL, l1_text(l->s.way ? L1T_NET_STEAMID : L1T_NET_ADDR, lang),
            l->s.join_addr[0] ? l->s.join_addr : l1_text(l->s.way ? L1T_NET_STEAMID_HINT : L1T_NET_ADDR_HINT, lang));
        ROW(A_NET_GO, L1V_NORMAL, l1_text(busy ? L1T_NET_STOP : L1T_NET_START, lang), "");
        ROW(A_NONE, role_locked(l) ? L1V_INFO : L1V_GREY, l1_text(L1T_YOU_PLAY, lang), role_locked(l) ? who1 : "-");   /* the host decides */
        snprintf(v, sizeof v, "< %s >", l1_text(l->ready ? L1T_ON : L1T_OFF, lang));
        ROW(connected && !started ? A_NET_READY : A_NONE, connected && !started ? L1V_NORMAL : L1V_GREY, l1_text(L1T_NET_READY, lang), v);
    }
    if (mode == L1M_HOST || mode == L1M_JOIN) {   /* round 2: the way (Direct IP / Steam friends) and the Steam host's invite, last so that the rows above keep their places */
        if (mode == L1M_HOST && l->s.way && busy && !started) ROW(A_NET_INVITE, L1V_NORMAL, l1_text(L1T_NET_INVITE, lang), "");
        snprintf(v, sizeof v, "< %s >", l1_text(l->s.way ? L1T_WAY_STEAM : L1T_WAY_IP, lang));
        ROW(busy ? A_NONE : A_NET_WAY, busy ? L1V_GREY : L1V_NORMAL, l1_text(L1T_NET_WAY, lang), v);
        /* ep1-flow E12: the Downed / revive rule of the session (the host chooses until the START; the guest sees the host's choice), after the rows above so that they
         * keep their places */
        char dv[96];
        if (mode == L1M_HOST) {
            l1_downed_text(l->s.downed, lang, dv, sizeof dv);
            snprintf(v, sizeof v, started ? "%s" : "< %s >", dv);
            ROW(started ? A_NONE : A_DOWNED, started ? L1V_INFO : L1V_NORMAL, l1_text(L1T_DOWNED, lang), v);
        } else {
            int known = connected && l->host_flags >= 0;
            if (known) l1_downed_text(l1_downed_from_flags((unsigned)l->host_flags), lang, dv, sizeof dv);
            ROW(A_NONE, known ? L1V_INFO : L1V_GREY, l1_text(L1T_DOWNED, lang), known ? dv : "-");   /* the host decides */
        }
        /* rel010: the friend row (Join + Steam friends, before the start): the friends in the game with a lobby; OK joins the one shown. Last, so that the rows
         * above keep their places for the lab scripts. */
        if (mode == L1M_JOIN && l->s.way && !busy) {
            int nf = l->env.n_friends > L1_FRIENDS ? L1_FRIENDS : l->env.n_friends;
            if (nf > 0) {
                int fs = l->friend_sel >= 0 && l->friend_sel < nf ? l->friend_sel : 0;
                snprintf(v, sizeof v, nf > 1 ? "< %s >" : "%s", l->env.friend_name[fs][0] ? l->env.friend_name[fs] : l->env.friend_id[fs]);
                ROW(A_NET_FRIEND, L1V_NORMAL, l1_text(L1T_NET_FRIEND, lang), v);
            } else
                ROW(A_NONE, L1V_GREY, l1_text(L1T_NET_FRIEND, lang), l1_text(L1T_NET_NOFRIEND, lang));
        }
    }
    ROW(A_DONE, L1V_NORMAL, l1_text(L1T_DONE, lang), "");
    return n;
}
#undef ROW

static int selectable(int action)
{
    return action == A_ARMED || action == A_ROLE || action == A_LAYOUT || action == A_PADS || action == A_DONE || action == A_NET_MODE || action == A_NET_ADDR ||
           action == A_NET_GO || action == A_NET_READY || action == A_MODE || action == A_KICK || action == A_NET_WAY || action == A_NET_INVITE || action == A_DOWNED || action == A_NET_FRIEND;
}

static void step(l1_t *l, int dir)
{
    l1_row rows[L1_ROWS];
    int n = build_rows(l, rows);
    for (int i = 0; i < n; i++) {
        l->sel = (l->sel + dir + n) % n;
        if (selectable(rows[l->sel].action)) return;
    }
}

/* Start / Disconnect: a request for the glue. Start needs a host, or a join address that is well formed. */
static void net_go(l1_t *l)
{
    if (link_active(l)) {
        l->request = L1R_NET_STOP;
        return;
    }
    if (l->s.net_mode == L1N_OFF) return;
    if (l->s.net_mode == L1N_JOIN && !l1_addr_ok(l->s.join_addr)) return;   /* the status line says what is wrong */
    l->request = L1R_NET_START;
}

static void change(l1_t *l, int action, int dir)   /* dir: -1 left, +1 right or confirm */
{
    switch (action) {
    case A_ARMED: l->s.armed = !l->s.armed; l->changed = 1; break;   /* every local setting has two values: left, right and confirm all toggle */
    case A_ROLE: l->s.role = l->s.role == L1_LEAD ? L1_PARTNER : L1_LEAD; l->changed = 1; break;
    case A_LAYOUT: l->s.layout = l->s.layout == L1_LAYOUT_SIDE ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE; l->changed = 1; break;
    case A_PADS: l->s.pads_swapped = !l->s.pads_swapped; l->changed = 1; break;
    case A_NET_MODE: l->s.net_mode = (l->s.net_mode + dir + 3) % 3; l->changed = 1; break;   /* Off, Host, Join */
    case A_MODE: l1_mode_set(&l->s, (l1_mode_of(&l->s) + dir + 4) % 4); l->changed = 1; break;   /* Off, Split screen, Network: host, Network: join */
    case A_NET_GO: net_go(l); break;
    case A_NET_READY: l->ready = !l->ready; break;
    case A_NET_WAY: l->s.way = !l->s.way; l->changed = 1; break;
    case A_DOWNED: l->s.downed = downed_step(l->s.downed, dir); l->changed = 1; break;   /* Off, 30 s, 60 s, 120 s, no time limit */
    case A_NET_INVITE:
        if (dir > 0) l->request = L1R_NET_INVITE;
        break;
    case A_NET_FRIEND: {   /* rel010: left / right picks a friend; OK / click joins him (his SteamID is the join target, as if typed) */
        int nf = l->env.n_friends > L1_FRIENDS ? L1_FRIENDS : l->env.n_friends;
        if (nf <= 0) break;
        if (l->friend_sel < 0 || l->friend_sel >= nf) l->friend_sel = 0;
        if (l->evt_ok) {
            snprintf(l->s.join_addr, sizeof l->s.join_addr, "%s", l->env.friend_id[l->friend_sel]);
            l->changed = 1;
            net_go(l);
        } else
            l->friend_sel = (l->friend_sel + dir + nf) % nf;
        break;
    }
    case A_KICK:
        if (dir < 0) break;
        if (!l->kick_ask) l->kick_ask = 1;
        else l->kick_ask = 0, l->request = L1R_NET_KICK;
        break;
    default: break;
    }
}

void l1_event_in(l1_t *l, const l1_event *e)
{
    l1_row rows[L1_ROWS];
    int n = build_rows(l, rows);
    if (l->sel < 0 || l->sel >= n) l->sel = 0;
    int before = l->sel;
    switch (e->kind) {
    case L1E_UP: step(l, -1); break;
    case L1E_DOWN: step(l, 1); break;
    case L1E_LEFT:
        if (rows[l->sel].action != A_NET_GO && rows[l->sel].action != A_NET_ADDR && rows[l->sel].action != A_KICK) change(l, rows[l->sel].action, -1);   /* Start and the address are not toggles */
        break;
    case L1E_RIGHT:
        if (rows[l->sel].action != A_NET_GO && rows[l->sel].action != A_NET_ADDR && rows[l->sel].action != A_KICK) change(l, rows[l->sel].action, 1);
        break;
    case L1E_OK:
        if (rows[l->sel].action == A_DONE) l->request = L1R_CLOSE;
        else l->evt_ok = 1, change(l, rows[l->sel].action, 1), l->evt_ok = 0;
        break;
    case L1E_BACK:
        if (l->kick_ask) l->kick_ask = 0;   /* Back takes the question back first */
        else l->request = L1R_CLOSE;
        break;
    case L1E_CHAR:   /* typing goes to the join address only */
        if (rows[l->sel].action == A_NET_ADDR && e->ch > 0x20 && e->ch < 0x7F) {
            size_t len = strlen(l->s.join_addr);
            if (len + 1 < sizeof l->s.join_addr) {
                l->s.join_addr[len] = (char)e->ch;
                l->s.join_addr[len + 1] = 0;
                l->changed = 1;
            }
        }
        break;
    case L1E_BACKSPACE:
        if (rows[l->sel].action == A_NET_ADDR && l->s.join_addr[0]) {
            l->s.join_addr[strlen(l->s.join_addr) - 1] = 0;
            l->changed = 1;
        }
        break;
    case L1E_HOVER:
        if (e->row >= 0 && e->row < n && selectable(rows[e->row].action)) l->sel = e->row;
        break;
    case L1E_CLICK:
        if (e->row >= 0 && e->row < n && selectable(rows[e->row].action)) {
            l->sel = e->row;
            if (rows[e->row].action == A_DONE) l->request = L1R_CLOSE;
            else l->evt_ok = 1, change(l, rows[e->row].action, 1), l->evt_ok = 0;
        }
        break;
    default: break;
    }
    if (l->kick_ask && (l->sel >= n || rows[l->sel].action != A_KICK)) l->kick_ask = 0;   /* moving away takes the question back */
    (void)before;
}

int l1_take_request(l1_t *l)
{
    int r = l->request;
    l->request = L1R_NONE;
    if (r == L1R_CLOSE) l->open = 0;
    return r;
}

int l1_changed(l1_t *l)
{
    int c = l->changed;
    l->changed = 0;
    return c;
}

void l1_settings_get(l1_t *l, l1_settings *s) { *s = l->s; }

/* ---------------------------------------------------------------- texts of the network game */
static void join2(char *out, size_t size, const char *a, const char *b)
{
    if (a[0] && b[0]) snprintf(out, size, "%s  %s", a, b);
    else snprintf(out, size, "%s", a[0] ? a : b);
}

void l1_reason_text(int reason, int lang, const char *a1, const char *a2, char *out, size_t size)
{
    char t[400];
    if (!size) return;
    out[0] = 0;
    a1 = a1 ? a1 : "", a2 = a2 ? a2 : "";
    switch (reason) {
    case L1RS_FAILED: snprintf(out, size, "%s", l1_text(L1T_NET_ST_ERROR, lang)); break;
    case L1RS_NOANSWER: l1_fmt(out, size, l1_text(L1T_REF_NOANSWER, lang), a1[0] ? a1 : "?", NULL); break;
    case L1RS_FULL: snprintf(out, size, "%s", l1_stock(L1K_FULL)); break;
    case L1RS_STARTED: snprintf(out, size, "%s", l1_text(L1T_REF_STARTED, lang)); break;
    case L1RS_PROTO: snprintf(out, size, "%s", l1_text(L1T_REF_PROTO, lang)); break;
    case L1RS_MODVER: l1_fmt(out, size, l1_text(L1T_REF_MODVER, lang), a1[0] ? a1 : "?", a2[0] ? a2 : "?"); break;
    case L1RS_MODVER_OLD: l1_fmt(out, size, l1_text(L1T_REF_MODVER_OLD, lang), a2[0] ? a2 : "?", NULL); break;
    case L1RS_BUILD: snprintf(out, size, "%s", l1_text(L1T_REF_BUILD, lang)); break;
    case L1RS_REMOVED: snprintf(out, size, "%s", l1_stock(L1K_REMOVED)); break;
    case L1RS_HOST_ENDED: snprintf(out, size, "%s", l1_text(L1T_HOST_ENDED, lang)); break;
    case L1RS_HOST_LOST: snprintf(out, size, "%s", l1_stock(L1K_LOST)); break;
    case L1RS_PARTNER_LEFT: l1_fmt(out, size, l1_stock(L1K_LEFT), a1[0] ? a1 : l1_text(L1T_PLAYER2, lang), NULL); break;
    case L1RS_PARTNER_LOST: snprintf(out, size, "%s", l1_stock(L1K_LOST)); break;
    default: break;
    }
    (void)t;
}

/* the network status line: what the link is doing and what the player can do next */
static void net_status(const l1_t *l, char *out, size_t size, int *kind)
{
    int lang = l->env.lang, host = l->s.net_mode == L1N_HOST;
    char a[400], b[400], who[120];
    *kind = L1S_INFO;
    out[0] = 0;
    if (!l->env.net_rows) {   /* the local screen with the old network rows is gone: nothing to say */
        return;
    }
    if (l->s.net_mode == L1N_OFF) return;
    if (l->started && l->net_state == L1NS_CONNECTED) {   /* after the START: what happens next */
        if (host) {
            l1_expand(l1_text(L1T_NET_ST_GO_HOST, lang), lang, a, sizeof a);
            l1_fmt(out, size, a, peer_word(l), NULL);
        } else
            l1_fmt(out, size, l1_text(L1T_NET_ST_GO_GUEST, lang), peer_word(l), NULL);
        *kind = L1S_OK;
        return;
    }
    switch (l->net_state) {
    case L1NS_WAIT:
        if (l->reason == L1RS_PARTNER_LEFT || l->reason == L1RS_PARTNER_LOST) {   /* the partner went away: say so, the host keeps listening */
            l1_reason_text(l->reason, lang, l->ra1, l->ra2, a, sizeof a);
            join2(out, size, a, l1_text(L1T_NET_ST_WAIT, lang));
            *kind = L1S_WARN;
        } else
            snprintf(out, size, "%s", l1_text(L1T_NET_ST_WAIT, lang));
        break;
    case L1NS_CONNECTING: l1_fmt(out, size, l1_text(L1T_NET_ST_CONNECTING_TO, lang), l->s.join_addr, NULL); break;
    case L1NS_CONNECTED:
        if (host) l1_fmt(a, sizeof a, l1_stock(L1K_JOINED), peer_word(l), NULL);
        else if (l->net_role >= 0) {
            who_text(l, l->net_role, who, sizeof who);
            l1_fmt(a, sizeof a, l1_text(L1T_JOINING_AS, lang), who, NULL);
        } else
            snprintf(a, sizeof a, "%s", l1_text(L1T_NET_ST_CONN, lang));
        if (l->ready && l->peer_ready) snprintf(b, sizeof b, "%s", l1_text(L1T_NET_ST_BOTH, lang)), *kind = L1S_OK;
        else if (l->ready) snprintf(b, sizeof b, "%s", l1_text(L1T_NET_ST_WAITREADY, lang));
        else if (l->peer_ready) {
            char r[200];
            l1_fmt(r, sizeof r, l1_stock(L1K_READY), peer_word(l), NULL);
            join2(b, sizeof b, r, l1_text(L1T_NET_ST_MARK, lang));
        } else
            snprintf(b, sizeof b, "%s", host || l->net_role >= 0 ? l1_text(L1T_NET_ST_MARK, lang) : "");
        join2(out, size, a, b);
        break;
    case L1NS_ERROR:
        l1_reason_text(l->reason ? l->reason : L1RS_FAILED, lang, l->ra1, l->ra2, out, size);
        if (!out[0]) snprintf(out, size, "%s", l1_text(L1T_NET_ST_ERROR, lang));
        *kind = L1S_WARN;
        break;
    default:   /* not started */
        if (host) snprintf(out, size, "%s", l1_text(L1T_NET_ST_HOST, lang));
        else if (l->s.join_addr[0] && !l1_addr_ok(l->s.join_addr)) snprintf(out, size, "%s", l1_text(L1T_NET_ST_BADADDR, lang)), *kind = L1S_WARN;
        else snprintf(out, size, "%s", l1_text(L1T_NET_ST_JOIN, lang));
        break;
    }
}

/* an Episode 1 area: 0x9101SS00 (the episode byte of the campaign id) */
static int ep1_area(unsigned a) { return (a & 0xFFFF0000u) == 0x91010000u; }

/* the selection never rests on a row that cannot be used (lm1 live: a lobby opened on a running link had the locked Mode row selected) */
static void sel_fix(l1_t *l, const l1_row *rows, int n)
{
    if (l->sel < 0 || l->sel >= n) l->sel = 0;
    for (int i = 0; i < n && !selectable(rows[l->sel].action); i++) l->sel = (l->sel + 1) % n;
}

void l1_view_get(l1_t *l, l1_view *v)
{
    int lang = l->env.lang, mode = l1_mode_of(&l->s), net = l->env.net_rows && (mode == L1M_HOST || mode == L1M_JOIN);
    memset(v, 0, sizeof *v);
    snprintf(v->title, sizeof v->title, "%s", l1_text(L1T_TITLE, lang));
    snprintf(v->subtitle, sizeof v->subtitle, "%s", l1_text(!l->env.net_rows || mode == L1M_SPLIT ? L1T_SUBTITLE : net ? L1T_SUBTITLE_NET : L1T_ROW_HELP, lang));
    v->nrows = build_rows(l, v->rows);
    sel_fix(l, v->rows, v->nrows);
    v->sel = l->sel;
    snprintf(v->hint, sizeof v->hint, "%s", l1_text(L1T_HINT, lang));
    snprintf(v->leg_ok, sizeof v->leg_ok, "%s", l1_text(L1T_LEG_OK, lang));
    snprintf(v->leg_back, sizeof v->leg_back, "%s", l1_text(L1T_LEG_BACK, lang));
    v->pads = l->env.pads_connected;
    net_status(l, v->net_status, sizeof v->net_status, &v->net_status_kind);
    l1_assign a;
    l1_settings shown = l->s;
    shown.role = eff_role(l);
    l1_assign_get(&shown, &a);
    const char *lead = lead_name(l), *part = partner_name(l);
    if (net) {   /* the two cards: this PC's human and the partner on the other PC */
        int host = mode == L1M_HOST, role = eff_role(l), known = host || role_locked(l);
        v->cards_on = 1;
        snprintf(v->card[0].who, sizeof v->card[0].who, "%s", l1_text(host ? L1T_PLAYER1 : L1T_PLAYER2, lang));
        snprintf(v->card[1].who, sizeof v->card[1].who, "%s", l1_text(host ? L1T_PLAYER2 : L1T_PLAYER1, lang));
        snprintf(v->card[0].chr, sizeof v->card[0].chr, "%s", known ? (role == L1_PARTNER ? part : lead) : "-");
        snprintf(v->card[1].chr, sizeof v->card[1].chr, "%s", known ? (role == L1_PARTNER ? lead : part) : "-");
        snprintf(v->card[0].pad, sizeof v->card[0].pad, "%s", l1_text(L1T_PAD1, lang));
        if (l->net_state == L1NS_CONNECTED) l1_fmt(v->card[1].pad, sizeof v->card[1].pad, l1_text(L1T_NET_PEER, lang), peer_word(l), NULL);
        else snprintf(v->card[1].pad, sizeof v->card[1].pad, "%s", l1_stock(L1K_WAITING));
        if (l->net_state == L1NS_CONNECTED) {   /* ready1: each player's ready state on its card (Jericho's seats: "<name>: <character> - READY"), the game's Raid lobby words */
            v->card[0].ready_on = l->ready ? 2 : 1, v->card[1].ready_on = l->peer_ready ? 2 : 1;
            snprintf(v->card[0].ready, sizeof v->card[0].ready, "%s", l1_stock(l->ready ? L1K_PARTNER_READY : L1K_PARTNER_WAIT));
            snprintf(v->card[1].ready, sizeof v->card[1].ready, "%s", l1_stock(l->peer_ready ? L1K_PARTNER_READY : L1K_PARTNER_WAIT));
        }
        snprintf(v->status, sizeof v->status, "%s", v->net_status);   /* the network line is the status of a network mode */
        v->status_kind = v->net_status_kind;
        snprintf(v->note, sizeof v->note, "%s", l1_text(L1T_NOTE_SAVE_NET, lang));
        char t[400];
        if (host && l->env.save_area && !ep1_area(l->env.save_area)) {   /* Continue would leave Episode 1: say it before the start */
            l1_expand(l1_text(L1T_NET_EP_ONLY, lang), lang, t, sizeof t);
            snprintf(v->note2, sizeof v->note2, "%s", t);
        } else if (host && l->net_state == L1NS_WAIT && l->env.alone_s >= 30)   /* nobody came: the internet case */
            snprintf(v->note2, sizeof v->note2, "%s", l1_text(L1T_NET_HOST_HINT, lang));
        else if (host && (l->env.coop_role1 == 1 || l->env.coop_role1 == 2)) {   /* rev1 live 16: Continue plays the co-op save's characters, New Game the room's */
            int sp = l->env.coop_role1 == 2;
            l1_expand(l1_text(L1T_NOTE_CONT_ROLES, lang), lang, t, sizeof t);
            l1_fmt(v->note2, sizeof v->note2, t, sp ? part : lead, sp ? lead : part);
        } else
            snprintf(v->note2, sizeof v->note2, "%s", l1_text(L1T_RAID_NOTE, lang));
        return;
    }
    v->cards_on = l->s.armed;
    snprintf(v->card[0].who, sizeof v->card[0].who, "%s", l1_text(L1T_PLAYER1, lang));
    snprintf(v->card[1].who, sizeof v->card[1].who, "%s", l1_text(L1T_PLAYER2, lang));
    snprintf(v->card[0].chr, sizeof v->card[0].chr, "%s", a.unit1 == 0 ? lead : part);
    snprintf(v->card[1].chr, sizeof v->card[1].chr, "%s", a.unit2 == 0 ? lead : part);
    snprintf(v->card[0].pad, sizeof v->card[0].pad, "%s", l1_text(a.swap ? L1T_PAD2 : L1T_PAD1, lang));
    snprintf(v->card[1].pad, sizeof v->card[1].pad, "%s", l1_text(a.swap ? L1T_PAD1 : L1T_PAD2, lang));
    if (l->s.armed) {
        if (l->env.pads_connected <= 0) {
            snprintf(v->status, sizeof v->status, "%s", l1_text(L1T_WARN_NOPAD, lang));
            v->status_kind = L1S_WARN;
        } else if (l->env.pads_connected == 1) {
            snprintf(v->status, sizeof v->status, "%s", l1_text(L1T_WARN_ONEPAD, lang));
            v->status_kind = L1S_WARN;
        } else {
            l1_expand(l1_text(L1T_ST_ON, lang), lang, v->status, sizeof v->status);
            v->status_kind = L1S_OK;
        }
        snprintf(v->note, sizeof v->note, "%s  %s", l1_text(L1T_NOTE_SAVE, lang), l1_text(L1T_NOTE_SOLO, lang));
    } else {
        snprintf(v->status, sizeof v->status, "%s", l1_text(L1T_ST_OFF, lang));
        v->status_kind = L1S_INFO;
    }
    if (!l->env.section_known && l->s.armed)
        snprintf(v->note2, sizeof v->note2, "%s  %s", l1_text(L1T_NEXT_SECTION, lang), l1_text(L1T_RAID_NOTE, lang));
    else
        snprintf(v->note2, sizeof v->note2, "%s", l1_text(L1T_RAID_NOTE, lang));
}

/* "Episode 1": the game's own campaign text of the running language first, the built-in of the language when it was not loaded */
static const char *ep1_word(int lang) { return l1_stock_have(L1K_EPISODE1) ? l1_stock(L1K_EPISODE1) : l1_text(L1T_EPISODE1, lang); }

void l1_strip_text(const l1_strip_in *in, int lang, char *out, size_t size, int *kind)
{
    char a[400], b[400], secs[16];
    int k = L1S_INFO;
    if (!size) return;
    out[0] = 0;
    if (lang < 0 || lang >= L1_LANGS) lang = 0;
    const char *name = in && in->name[0] ? in->name : l1_text(L1T_PLAYER2, lang);
    switch (in ? in->kind : L1G_NONE) {
    case L1G_WAIT_LOAD: l1_fmt(out, size, l1_text(L1T_WAIT_LOAD, lang), name, NULL); break;
    case L1G_SILENT:
        snprintf(secs, sizeof secs, "%d", in->seconds);
        l1_fmt(out, size, l1_text(L1T_PARTNER_SILENT, lang), name, secs);
        k = L1S_WARN;
        break;
    case L1G_PARTNER_LEFT:
    case L1G_PARTNER_LOST:
        l1_reason_text(in->kind == L1G_PARTNER_LEFT ? L1RS_PARTNER_LEFT : L1RS_PARTNER_LOST, lang, name, NULL, a, sizeof a);
        join2(out, size, a, l1_text(L1T_CONTINUES_ALONE, lang));
        k = L1S_WARN;
        break;
    case L1G_HOST_ENDED:
    case L1G_HOST_LOST:
    case L1G_REMOVED:
        l1_reason_text(in->kind == L1G_HOST_ENDED ? L1RS_HOST_ENDED : in->kind == L1G_HOST_LOST ? L1RS_HOST_LOST : L1RS_REMOVED, lang, NULL, NULL, a, sizeof a);
        join2(out, size, a, l1_text(L1T_TO_TITLE, lang));
        k = L1S_WARN;
        break;
    case L1G_GUEST_HOLD: l1_fmt(out, size, l1_text(L1T_NET_ST_GO_GUEST, lang), name, NULL); break;
    case L1G_HOST_GO:
        l1_expand(l1_text(L1T_NET_ST_GO_HOST, lang), lang, b, sizeof b);
        l1_fmt(out, size, b, name, NULL);
        break;
    case L1G_EP_ENDED: snprintf(out, size, "%s", l1_text(L1T_EP_ENDED, lang)), k = L1S_WARN; break;
    case L1G_EP1_DONE: l1_fmt(out, size, l1_text(L1T_EP1_DONE, lang), ep1_word(lang), NULL); break;   /* ep1-flow E5 */
    case L1G_EP1_BEHIND:
        l1_fmt(a, sizeof a, l1_text(L1T_EP1_BEHIND, lang), ep1_word(lang), NULL);
        join2(out, size, a, l1_text(L1T_TO_TITLE, lang));
        k = L1S_WARN;
        break;
    case L1G_SOLO_WATCH: l1_fmt(out, size, l1_text(L1T_SOLO_WATCH, lang), name, NULL); break;   /* ep1-solo E3 */
    case L1G_CONT_ROLES:   /* rev1 live 16: the host's Continue put the co-op save's roles back */
        l1_expand(l1_text(L1T_CONT_ROLES, lang), lang, b, sizeof b);
        l1_fmt(out, size, b, name, NULL);
        break;
    case L1G_READY_GATE: {   /* ready1: the story waits for both ready flags (Jericho: the host's Start needs every seat ready); the lobby's own lines, then where to mark ready */
        int f = in->flags, host = (f & L1GF_HOST) != 0;
        if (!(f & L1GF_CONNECTED)) snprintf(a, sizeof a, "%s", l1_text(host ? L1T_NET_ST_WAIT : L1T_NET_ST_CONNECTING, lang));
        else if ((f & L1GF_READY) && (f & L1GF_PEER_READY)) snprintf(a, sizeof a, "%s", l1_text(L1T_NET_ST_BOTH, lang));
        else if (f & L1GF_READY) snprintf(a, sizeof a, "%s", l1_text(L1T_NET_ST_WAITREADY, lang));
        else if (f & L1GF_PEER_READY) {
            l1_fmt(b, sizeof b, l1_stock(L1K_READY), name, NULL);
            join2(a, sizeof a, b, l1_text(L1T_NET_ST_MARK, lang));
        } else
            snprintf(a, sizeof a, "%s", l1_text(L1T_NET_ST_MARK, lang));
        join2(out, size, a, (f & L1GF_CONNECTED) && (f & L1GF_READY) ? "" : l1_text(L1T_HINT_TITLE, lang));
        k = L1S_WARN;
        break;
    }
    default: break;
    }
    if (kind) *kind = k;
}

void l1_hint_text(const l1_settings *s, const l1_env *env, char *out, size_t size)
{
    int lang = env ? env->lang : 0;
    if (lang < 0 || lang >= L1_LANGS) lang = 0;
    if (!s->armed) {
        snprintf(out, size, "%s", l1_text(L1T_HINT_TITLE, lang));
        return;
    }
    l1_assign a;
    l1_assign_get(s, &a);
    const char *lead = env && env->lead[0] ? env->lead : l1_person_name(0, lang);
    const char *part = env && env->partner[0] ? env->partner : l1_person_name(2, lang);
    snprintf(out, size, "%s:  %s / %s    |    %s", l1_text(L1T_BANNER_ON, lang), a.unit1 == 0 ? lead : part, a.unit2 == 0 ? lead : part, l1_text(L1T_HINT_TITLE, lang));
}

/* ---------------------------------------------------------------- settings file */
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

int l1_ini_load(const char *path, l1_settings *s, int *lang_override)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "armed")) s->armed = atoi(v) != 0;
        else if (!strcmp(k, "role")) s->role = !strcmp(v, "partner") ? L1_PARTNER : L1_LEAD;
        else if (!strcmp(k, "layout")) s->layout = !strcmp(v, "top") ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE;
        else if (!strcmp(k, "pads")) s->pads_swapped = !strcmp(v, "swapped");
        else if (!strcmp(k, "join")) snprintf(s->join_addr, sizeof s->join_addr, "%s", v);
        else if (!strcmp(k, "way")) s->way = !strcmp(v, "steam");
        else if (!strcmp(k, "downed")) s->downed = !strcmp(v, "off") || !strcmp(v, "0") ? L1D_OFF : !strcmp(v, "30") ? L1D_30 : !strcmp(v, "120") ? L1D_120 : !strcmp(v, "never") ? L1D_NEVER : L1D_60;
        else if (!strcmp(k, "language") && lang_override) {
            int i = l1_lang_from_code(v);
            *lang_override = i;   /* -1 = auto */
        }
    }
    fclose(f);
    sanitize(s);
    return 1;
}

int l1_ini_save(const char *path, const l1_settings *s, int lang_override)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    fprintf(f, "# Resident Evil Revelations co-op mod: settings (written by the Co-Op Play screen)\r\n");
    fprintf(f, "armed=%d\r\nrole=%s\r\nlayout=%s\r\npads=%s\r\n", s->armed ? 1 : 0, s->role == L1_PARTNER ? "partner" : "lead", s->layout == L1_LAYOUT_TOPBOTTOM ? "top" : "side",
            s->pads_swapped ? "swapped" : "normal");
    if (s->join_addr[0]) fprintf(f, "join=%s\r\n", s->join_addr);
    fprintf(f, "way=%s\r\n", s->way ? "steam" : "ip");
    fprintf(f, "downed=%s\r\n", s->downed == L1D_OFF ? "off" : s->downed == L1D_30 ? "30" : s->downed == L1D_120 ? "120" : s->downed == L1D_NEVER ? "never" : "60");   /* network host: off|30|60|120|never */
    if (lang_override >= 0 && lang_override < L1_LANGS) fprintf(f, "language=%s\r\n", l1_lang_codes[lang_override]);
    if (fclose(f) != 0) return 0;
    remove(path);
    return rename(tmp, path) == 0;
}
