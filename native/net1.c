/*
 * net1.c - the network core of the Revelations 1 co-op mod (see net1.h).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

#include "coop_net.h"
#include "net1.h"

static net1_cfg g_cfg;
static int g_cfg_set;
static lk_t *g_link;
static l1_t *g_lobby;
static CRITICAL_SECTION g_cs;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
/* what the last frame logged, so that only changes reach the log */
static int g_logged_state = -1, g_logged_started, g_logged_phase = -1, g_logged_epoch = -1, g_logged_gos, g_logged_loads;
static char g_name[40];
static const char *(*g_persona)(void);
static int g_sess_console;   /* ep1-flow E12 (defined with the session flags below) */
static const char *(*g_fallback)(int side);   /* round 2: the localised fallback name (side 0 = host "Player 1", 1 = guest "Player 2") */
void net1_set_fallback(const char *(*fb)(int side)) { g_fallback = fb; }
static char g_mod[24];
static int g_no_rejoin;

static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o, (void)p, (void)c;
    InitializeCriticalSection(&g_cs);
    return TRUE;
}
static void lock(void)
{
    InitOnceExecuteOnce(&g_once, init_cs, NULL, NULL);
    EnterCriticalSection(&g_cs);
}
static void unlock(void) { LeaveCriticalSection(&g_cs); }

static void logf_(const char *fmt, ...)
{
    if (!g_cfg.log) return;
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_cfg.log(b);
}

int net1_parse_exe_tag(const char *hex, uint8_t tag[8])
{
    memset(tag, 0, 8);
    if (!hex) return 0;
    for (int i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)hex[i])) return 0;
    uint8_t t[8];
    for (int i = 0; i < 8; i++) {
        char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
        t[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    memcpy(tag, t, 8);
    return 1;
}

void net1_configure(const net1_cfg *c)
{
    lock();
    memset(&g_cfg, 0, sizeof g_cfg);
    if (c) g_cfg = *c;
    g_cfg_set = 1;
    cn_set_log(g_cfg.log);
    unlock();
}

/* the name the partner sees: the configured one (tests), REV1_NAME (lab), the Steam persona name, else "Player 1" / "Player 2" by the side this PC takes
 * (never the computer name or the Windows user name: they are personal data, the log collector scrubs both) */
void net1_name_for(int mode, char *out, size_t size)
{
    if (!size) return;
    out[0] = 0;
    if (g_cfg.name && g_cfg.name[0]) snprintf(out, size, "%s", g_cfg.name);
    else if (GetEnvironmentVariableA("REV1_NAME", out, (DWORD)size) == 0 || !out[0]) {
        const char *p = g_persona ? g_persona() : NULL;
        const char *fb = g_fallback ? g_fallback(mode == L1N_JOIN ? 1 : 0) : NULL;   /* the localised "Player 1" / "Player 2" of this PC's language */
        if (p && p[0]) snprintf(out, size, "%s", p);
        else if (fb && fb[0]) snprintf(out, size, "%s", fb);
        else snprintf(out, size, "%s", mode == L1N_JOIN ? "Player 2" : "Player 1");
    }
    /* the session keeps CP_NAME_MAX bytes: cut there at a character boundary (a long Cyrillic / CJK persona would end in half a character) */
    size_t n = strlen(out);
    if (n > CP_NAME_MAX) {
        n = CP_NAME_MAX;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
        out[n] = 0;
    }
    for (char *c = out; *c; c++)
        if ((unsigned char)*c < 0x20) *c = ' ';
}

static void default_name(void) { net1_name_for(L1N_HOST, g_name, sizeof g_name); }

void net1_set_identity(const char *(*persona)(void), const char *mod_version, int no_rejoin)
{
    lock();
    g_persona = persona;
    snprintf(g_mod, sizeof g_mod, "%s", mod_version ? mod_version : "");
    g_no_rejoin = no_rejoin != 0;
    if (g_link) lk_set_no_rejoin(g_link, g_no_rejoin);
    unlock();
}

static lk_t *link_u(void)
{
    if (g_link) return g_link;
    if (!g_cfg_set) memset(&g_cfg, 0, sizeof g_cfg), g_cfg_set = 1;
    default_name();
    lk_config lc;
    memset(&lc, 0, sizeof lc);
    lc.name = g_name;
    lc.mod_version = g_mod[0] ? g_mod : NULL;
    lc.no_rejoin = g_no_rejoin;
    net1_parse_exe_tag(g_cfg.exe_sha, lc.exe_tag);
    lc.clock = g_cfg.clock, lc.clock_user = g_cfg.clock_user;
    g_link = lk_new_cfg(g_lobby, g_cfg.transport ? g_cfg.transport : "enet", g_cfg.threaded, &lc);
    if (g_link)
        logf_("net: link created, transport %s, mod %s, rejoin after the start %s, exe tag %02x%02x%02x%02x%02x%02x%02x%02x", g_cfg.transport ? g_cfg.transport : "enet",
              g_mod[0] ? g_mod : "(default)", g_no_rejoin ? "refused" : "allowed", lc.exe_tag[0], lc.exe_tag[1], lc.exe_tag[2], lc.exe_tag[3], lc.exe_tag[4], lc.exe_tag[5], lc.exe_tag[6],
              lc.exe_tag[7]);
    return g_link;
}

lk_t *net1_link(void)
{
    lock();
    lk_t *k = link_u();
    unlock();
    return k;
}
lk_t *net1_link_peek(void) { return g_link; }

void net1_attach_lobby(l1_t *lobby)
{
    lock();
    g_lobby = lobby;
    if (g_link) lk_set_lobby(g_link, lobby);
    unlock();
}

static void reset_logged(void)
{
    g_logged_state = -1, g_logged_started = 0, g_logged_phase = g_logged_epoch = -1, g_logged_gos = g_logged_loads = 0;
}

int net1_start(int mode, const char *target, int port, char *err, size_t esz)
{
    char local[160];
    if (!err || !esz) err = local, esz = sizeof local;
    err[0] = 0;
    lock();
    lk_t *k = link_u();
    int ok = 0;
    if (!k) snprintf(err, esz, "cannot create the session");
    else {
        char own[48], nm[40];
        net1_name_for(mode, nm, sizeof nm);   /* the persona may be known only now (Steam is up after the title) */
        lk_set_name(k, nm);
        {   /* where the name came from, never the name itself (personal data stays out of the log) */
            const char *p = g_persona ? g_persona() : NULL;
            char envn[8];
            const char *src = g_cfg.name && g_cfg.name[0] ? "configured" : GetEnvironmentVariableA("REV1_NAME", envn, sizeof envn) ? "REV1_NAME" : p && p[0] ? "the Steam persona" : "the fallback";
            logf_("net: the name the partner sees comes from %s (%u bytes)", src, (unsigned)strlen(nm));
        }
        g_sess_console = -1;   /* ep1-flow: a console `net set flags` lasts one link */
        ok = lk_start(k, mode, target, port > 0 ? port : LK_DEFAULT_PORT, own, sizeof own, err, esz);
        reset_logged();   /* the next frame logs the state again */
        logf_("net: %s %s%s", mode == L1N_HOST ? "host" : "join", ok ? "started: " : "failed: ", ok ? (mode == L1N_JOIN ? (target ? target : "") : (own[0] ? own : "address unknown")) : err);
    }
    unlock();
    return ok;
}

void net1_stop(void)
{
    lock();
    if (g_link) {
        lk_stop(g_link);
        logf_("net: stopped");
    }
    unlock();
}

void net1_own_addr(char *out, size_t size)
{
    if (!size) return;
    out[0] = 0;
    lock();
    if (g_link) lk_own_addr(g_link, out, size);
    unlock();
}

int net1_session_active(void)
{
    int r = 0;
    lock();
    if (g_link && lk_mode(g_link) != L1N_OFF && lk_active(g_link)) {
        lk_info in;
        lk_get_info(g_link, &in);
        r = in.started != 0;
    }
    unlock();
    return r;
}

/* ep1-flow E12: the host's session flags byte (cp_session.flags: bit 0 the downed / revive rule, bits 1..2 its bleed-out preset; downed_logic.h). The product
 * default is the rule ON with 60 s (= 1); menu.c sets the lobby row's choice; REV1_DOWNED=0 / =1 and REV1_DOWNED_BLEED=30|60|120|0 override it (lab). The
 * guest's downed1 takes the host's byte from the session record at the START. */
static int g_sess_flags = 1, g_sess_tried = -1, g_sess_console = -1;   /* g_sess_console: `net set flags <n>` (lab) wins over the lobby and the env until the next start */
static int g_env_read, g_env_on = -1, g_env_preset = -1;   /* REV1_DOWNED: -1 not set, 0 off, 1 on; REV1_DOWNED_BLEED: -1 not set, else the preset 0..3 */
static unsigned g_sess_try_t;
static int sess_flags_env(int want)
{
    if (!g_env_read) {
        char v[8] = "", b[8] = "";
        g_env_read = 1;
        if (GetEnvironmentVariableA("REV1_DOWNED", v, sizeof v)) g_env_on = v[0] == '0' ? 0 : v[0] == '1' ? 1 : -1;
        if (GetEnvironmentVariableA("REV1_DOWNED_BLEED", b, sizeof b)) {
            int s = atoi(b);
            g_env_preset = s == 30 ? 1 : s == 120 ? 2 : s == 60 ? 0 : s == 0 && b[0] == '0' ? 3 : -1;   /* 0 = never bleeds out */
        }
    }
    if (g_env_on == 0) return 0;
    int r = want & 0xFF;
    if (g_env_on == 1 || g_env_preset >= 0) r |= 1;
    if (g_env_preset >= 0) r = (r & ~6) | (g_env_preset << 1);
    return r;
}
static int sess_env_set(void) { return g_env_on >= 0 || g_env_preset >= 0; }
void net1_set_session_flags(int flags)
{
    lock();
    g_sess_flags = flags & 0xFF;
    unlock();
}
static int sess_want(void) { return g_sess_console >= 0 ? g_sess_console : sess_flags_env(g_sess_flags); }
int net1_session_flags(void) { return sess_want(); }

void net1_frame(void)
{
    lock();
    if (g_link && lk_mode(g_link) != L1N_OFF) {
        lk_poll(g_link);
        lk_info in;
        lk_get_info(g_link, &in);
        if (lk_mode(g_link) == L1N_HOST && lk_active(g_link) && !in.started) {   /* E12: the host's choice into the session record (only before the START: cs_set_session) */
            int want = sess_want();
            unsigned now = GetTickCount();
            if ((in.set.flags & 0xFF) != want && (want != g_sess_tried || now - g_sess_try_t > 2000u)) {
                lk_settings st = in.set;
                st.flags = want;
                int ok = lk_set_settings(g_link, &st);
                if (want != g_sess_tried || ok)
                    logf_("net: session flags %d -> %d (downed / revive %s, bleed-out preset %d%s)%s", in.set.flags, want, want & 1 ? "ON" : "off", (want >> 1) & 3,
                          sess_env_set() ? ", REV1_DOWNED / REV1_DOWNED_BLEED override the lobby" : "", ok ? "" : ": not accepted now (tried again)");
                g_sess_tried = want, g_sess_try_t = now;
                lk_get_info(g_link, &in);
            }
        }
        int st = lk_state(g_link);
        if (st != g_logged_state) {
            g_logged_state = st;
            const char *why = lk_last_reason(g_link);
            logf_("net: link state %d%s%s", st, why[0] ? ", reason: " : "", why);
        }
        if (in.started && !g_logged_started)
            logf_("net: session STARTED, this PC plays %s, other %s; episode %d difficulty %d flags %d area %u", in.role == 0 ? "lead" : "partner", in.remote_role == 0 ? "lead" : in.remote_role == 1 ? "partner" : "-",
                  in.set.episode, in.set.difficulty, in.set.flags, (unsigned)in.set.area_id);
        g_logged_started = in.started;
        if (in.phase != g_logged_phase || in.epoch != g_logged_epoch || in.gos != g_logged_gos || in.load_begins != g_logged_loads) {
            if (in.started) logf_("net: phase %d epoch %d (load begins %d, go %d, last go epoch %d)", in.phase, in.epoch, in.load_begins, in.gos, in.last_go_epoch);
            g_logged_phase = in.phase, g_logged_epoch = in.epoch, g_logged_gos = in.gos, g_logged_loads = in.load_begins;
        }
    }
    unlock();
}

int net1_kick(void)
{
    int r = 0;
    lock();
    if (g_link) r = lk_kick(g_link);
    if (r) logf_("net: the partner was removed by the host");
    unlock();
    return r;
}

/* round 2, the Steam way: "steam" (Valve lobby + relay, invites, no port forwarding) or "ip" / "enet" (direct IP); the link keeps its object, only the next start
 * uses the other transport. 1 = known name. */
static const char *const WAY_STEAM = "steam", *const WAY_IP = "enet";
int net1_set_way(const char *way)
{
    const char *t = !way ? NULL : !strcmp(way, "steam") ? WAY_STEAM : (!strcmp(way, "ip") || !strcmp(way, "enet")) ? WAY_IP : !strcmp(way, "sim") ? "sim" : NULL;
    if (!t) return 0;
    lock();
    if (!g_cfg_set) memset(&g_cfg, 0, sizeof g_cfg), g_cfg_set = 1;
    g_cfg.transport = t;
    if (g_link) lk_set_transport(g_link, t);
    unlock();
    logf_("net: the next start uses %s", t == WAY_STEAM ? "the Steam way (lobby + relay)" : t == WAY_IP ? "direct IP" : t);
    return 1;
}
const char *net1_way(void) { return g_cfg.transport && !strcmp(g_cfg.transport, "steam") ? "steam" : "ip"; }

/* a join target of the Steam way: a SteamID64 (digits), lobby:<digits> or pending */
int net1_steam_target_ok(const char *t)
{
    if (!t || !t[0]) return 0;
    if (!strcmp(t, "pending")) return 1;
    if (!strncmp(t, "lobby:", 6)) t += 6;
    size_t n = strlen(t);
    if (n < 1 || n > 20) return 0;
    for (size_t i = 0; i < n; i++)
        if (t[i] < '0' || t[i] > '9') return 0;
    return 1;
}

static volatile LONG g_autojoined;   /* rel010: an auto-join started a link that the menu has not shown yet */
int net1_take_autojoin(void) { return InterlockedExchange(&g_autojoined, 0) != 0; }

/* coop_steam's auto-join ("Join game" in the friends list, an invite, +connect_lobby) runs "join <target> [steam]" here */
int net1_run_line(const char *line, char *out, int size)
{
    char verb[16] = "", target[96] = "", tr[16] = "", err[160] = "";
    if (!out || size <= 0) return 0;
    out[0] = 0;
    if (!line || sscanf(line, "%15s %95s %15s", verb, target, tr) < 2 || strcmp(verb, "join")) {
        snprintf(out, (size_t)size, "net: not understood: %s", line ? line : "");
        return 0;
    }
    if (tr[0] && !net1_set_way(tr)) {
        snprintf(out, (size_t)size, "net: unknown way %s", tr);
        return 0;
    }
    int ok = net1_start(L1N_JOIN, target, 0, err, sizeof err);
    if (ok) InterlockedExchange(&g_autojoined, 1);
    snprintf(out, (size_t)size, "%s", ok ? "joining" : err);
    return ok;
}

void net1_shutdown(void)
{
    lock();
    if (g_link) lk_free(g_link);
    g_link = NULL;
    reset_logged();
    unlock();
}

/* ---------------------------------------------------------------- console */
static void help(char *out, size_t size)
{
    snprintf(out, size,
             "net host [port] | net join <ip[:port]> | net status | net stop | net ready [0|1] | net role lead|partner | net set episode|difficulty|flags <n> area|seed <hex> ... | "
             "net start [force] | net auto on|off | net load [hexarea] | net done | net kick | net rejoin on|off (lab: allow a rejoin after the start)");
}

void net1_cmd(const char *args, char *out, size_t size)
{
    char sub[24] = "", rest[160] = "";
    if (!size) return;
    out[0] = 0;
    sscanf(args ? args : "", "%23s %159[^\r\n]", sub, rest);
    char err[160] = "";
    if (!strcmp(sub, "way")) {   /* round 2: net way [steam|ip] */
        if (rest[0] && !net1_set_way(rest)) snprintf(out, size, "net way steam|ip (now %s)", net1_way());
        else snprintf(out, size, "net: the next start uses %s", !strcmp(net1_way(), "steam") ? "the Steam way: net host = a friends-only Steam lobby, net join <steamid64|lobby:<id>|pending>" : "direct IP");
        return;
    }
    if (!strncmp(sub, "steam", 5)) {   /* round 2: net steam | steamlobbies | steaminvite [id] | steamallow ... | steampipe msg|p2p (coop_steam.c through the session) */
        lock();
        lk_t *k = link_u();
        cs_t *s = k ? lk_session(k) : NULL;
        if (s) cs_command(s, args, out, size);
        else snprintf(out, size, "net: no session object");
        unlock();
        return;
    }
    if (!strcmp(sub, "host")) {
        int port = rest[0] ? atoi(rest) : 0;
        if (rest[0] && (port < 1 || port > 65535)) { snprintf(out, size, "net host: the port must be 1..65535"); return; }
        if (net1_start(L1N_HOST, "", port, err, sizeof err)) {
            char own[48];
            net1_own_addr(own, sizeof own);
            snprintf(out, size, "hosting on port %d%s%s (tell the other player this address)", port > 0 ? port : LK_DEFAULT_PORT, own[0] ? ", this PC: " : "", own);
        } else
            snprintf(out, size, "net host failed: %s", err);
    } else if (!strcmp(sub, "join")) {
        int steam_ok = !strcmp(net1_way(), "steam") && net1_steam_target_ok(rest);   /* the Steam way: a SteamID64, lobby:<id> or pending (lm6: lobby:<id> was refused) */
        if (!steam_ok && !l1_addr_ok(rest)) { snprintf(out, size, "net join: the address must look like 192.168.0.10:%d%s", LK_DEFAULT_PORT, !strcmp(net1_way(), "steam") ? " (Steam way: a Steam ID, lobby:<id> or pending)" : ""); return; }
        if (net1_start(L1N_JOIN, rest, 0, err, sizeof err))
            snprintf(out, size, "joining %s (net status shows the progress)", rest);
        else
            snprintf(out, size, "net join failed: %s", err);
    } else if (!strcmp(sub, "stop")) {
        net1_stop();
        snprintf(out, size, "link stopped");
    } else if (!sub[0] || !strcmp(sub, "status")) {
        lock();
        if (!g_link) snprintf(out, size, "link: off (never started)");
        else lk_status(g_link, out, size);
        unlock();
    } else if (!strcmp(sub, "help")) {
        help(out, size);
    } else {
        lock();
        lk_t *k = g_link;
        if (!k || lk_mode(k) == L1N_OFF) {
            snprintf(out, size, "net %s: the link is not running (net host | net join <ip[:port]>)", sub);
        } else if (!strcmp(sub, "ready")) {
            int on = rest[0] ? atoi(rest) != 0 : 1;
            lk_set_ready(k, on);
            snprintf(out, size, "ready %d%s", on, lk_state(k) == L1NS_CONNECTED ? "" : " (taken when the other player is connected)");
        } else if (!strcmp(sub, "role")) {
            if (!strcmp(rest, "lead") || !strcmp(rest, "partner")) {
                if (lk_mode(k) != L1N_HOST) snprintf(out, size, "net role: only the host chooses (the other player gets the other role)");
                else lk_set_role(k, !strcmp(rest, "lead") ? L1_LEAD : L1_PARTNER), snprintf(out, size, "the host plays %s", rest);
            } else
                snprintf(out, size, "net role lead|partner");
        } else if (!strcmp(sub, "set")) {
            lk_info in;
            lk_get_info(k, &in);
            lk_settings st = in.set;
            st.seed = 0;   /* 0 = keep */
            int n = 0;
            char key[24], val[24];
            const char *p = rest;
            int used = 0;
            while (sscanf(p, "%23s %n", key, &used) == 1) {
                p += used;
                if (sscanf(p, "%23s %n", val, &used) != 1) break;
                p += used;
                if (!strcmp(key, "episode")) st.episode = atoi(val), n++;
                else if (!strcmp(key, "difficulty")) st.difficulty = atoi(val), n++;
                else if (!strcmp(key, "flags")) st.flags = atoi(val), g_sess_console = st.flags & 0xFF, n++;   /* (ep1-flow: kept against the lobby's Downed / revive value) */
                else if (!strcmp(key, "area")) st.area_id = (uint32_t)strtoul(val, NULL, 16), n++;
                else if (!strcmp(key, "seed")) st.seed = (uint32_t)strtoul(val, NULL, 16), n++;
                else { snprintf(out, size, "net set: unknown setting '%s'", key); n = -1; break; }
            }
            if (n > 0) {
                if (lk_set_settings(k, &st)) snprintf(out, size, "settings sent: episode %d difficulty %d flags %d area %x seed %x", st.episode, st.difficulty, st.flags, (unsigned)st.area_id, (unsigned)st.seed);
                else snprintf(out, size, "net set: only the host can change the settings, and only before the start");
            } else if (n == 0)
                snprintf(out, size, "net set episode|difficulty|flags <n> area|seed <hex> ...");
        } else if (!strcmp(sub, "start")) {
            if (lk_start_session(k, !strcmp(rest, "force"), err, sizeof err)) snprintf(out, size, "session started");
            else snprintf(out, size, "net start: %s", err);
        } else if (!strcmp(sub, "auto")) {
            lk_set_manual_start(k, !strcmp(rest, "off"));
            snprintf(out, size, "automatic start when both are ready: %s", lk_manual_start(k) ? "off" : "on");
        } else if (!strcmp(sub, "load")) {
            lk_load_begin(k, rest[0] ? (uint32_t)strtoul(rest, NULL, 16) : 0);
            snprintf(out, size, "load begins");
        } else if (!strcmp(sub, "done")) {
            lk_load_done(k);
            snprintf(out, size, "load done");
        } else if (!strcmp(sub, "kick")) {
            int r = lk_kick(k);
            snprintf(out, size, "%s", r ? "the partner was removed" : "net kick: only the host, before the start, with a partner in the lobby");
        } else if (!strcmp(sub, "rejoin")) {
            g_no_rejoin = strcmp(rest, "on") != 0;
            lk_set_no_rejoin(k, g_no_rejoin);
            snprintf(out, size, "rejoin after the start: %s", g_no_rejoin ? "refused (product)" : "allowed (lab)");
        } else
            help(out, size);
        unlock();
    }
}
