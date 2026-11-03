/*
 * Test console, REV1_CONSOLE=<port> (the tests use 28300; Revelations 2 uses 28200): UDP on 127.0.0.1 only (never another interface). One text command per
 * datagram, one text datagram back. Runs on its own thread: commands that read game memory use ReadProcessMemory on ourselves (a bad address answers
 * "unreadable" instead of crashing the game); commands that touch game state are handed to the game's logic thread (lt.c, probe.c).
 *
 *   ping | version | help | base | stub | frames | window | shot | quit | iat | threads | stacks | mainstack
 *   readmem <hexaddr> [len<=256]   readstr <hexaddr> [max<=200]   dword <hexaddr>   dump <a> <len> <file>   classname <hexaddr>   dtiname <hexaddr>   objscan
 *   pad / padtap / padhold <xinput user> ...   key / keydown / keyup / wmkey   mouse / mbtn / cursor   dinput   pads   steam   post   log <text>
 *   game thread: L <cmd>, players [v], unit, mode, gamepad, camera, cap on|off|set|status, ctl, call, w32, w8, wf, rf, probelog, ltinfo
 * Client: tools/r1console.py
 */
#include <winsock2.h>
#include <windows.h>
#include <xinput.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"

static unsigned long hex(const char *s) { return strtoul(s, NULL, 16); }

static void hexdump(const unsigned char *p, int n, char *out, int size)
{
    int o = 0;
    for (int i = 0; i < n && o + 4 < size; i++)
        o += snprintf(out + o, size - o, "%02x%s", p[i], (i & 15) == 15 && i + 1 < n ? "\n" : " ");
    if (n == 0)
        snprintf(out, size, "(empty)");
}

/* One thread: suspended only while its registers and stack are copied, then likely return addresses (a dword into any module's
 * code that follows a call) are printed as module+offset. Nothing allocates while it is suspended. */
static void where(DWORD addr, char *out, int size)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(out, size, "%s+%lx", b ? b + 1 : name, (unsigned long)(addr - (DWORD)m));
    } else
        snprintf(out, size, "%08lx", (unsigned long)addr);
}

static int thread_stack(DWORD tid, char *out, int size, int returns)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
        return snprintf(out, size, "thread %lu: cannot open\n", tid);
    CONTEXT c;
    DWORD ret[48], n = 0;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(h) == (DWORD)-1) {
        CloseHandle(h);
        return snprintf(out, size, "thread %lu: cannot suspend\n", tid);
    }
    int ok = GetThreadContext(h, &c);
    if (ok && returns) {
        DWORD stack[1024];
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (void *)c.Esp, stack, sizeof stack, &got);
        for (DWORD i = 0; i < got / 4 && n < 48; i++) {
            const unsigned char *p = (const unsigned char *)stack[i];
            unsigned char code[6];
            if (stack[i] < 0x10000 || !r1_safe_read(p - 6, code, 6))
                continue;
            if (code[1] == 0xE8 || (code[4] == 0xFF && (code[5] & 0x38) == 0x10) || (code[3] == 0xFF && (code[4] & 0x38) == 0x10) || (code[0] == 0xFF && (code[1] & 0x38) == 0x10))
                ret[n++] = stack[i];
        }
    }
    ResumeThread(h);
    CloseHandle(h);
    if (!ok)
        return snprintf(out, size, "thread %lu: no context\n", tid);
    char w[300];
    where(c.Eip, w, sizeof w);
    int o = snprintf(out, size, "thread %lu eip=%s esp=%08lx\n", tid, w, c.Esp);
    for (DWORD i = 0; i < n && o + 80 < size; i++) {
        where(ret[i], w, sizeof w);
        o += snprintf(out + o, size - o, "    ret %s\n", w);
    }
    return o;
}

static void all_threads(char *out, int size, int returns)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 t = {sizeof t};
    int o = 0;
    DWORD me = GetCurrentThreadId();
    if (snap == INVALID_HANDLE_VALUE) {
        snprintf(out, size, "no snapshot");
        return;
    }
    for (BOOL ok = Thread32First(snap, &t); ok && o + 200 < size; ok = Thread32Next(snap, &t))
        if (t.th32OwnerProcessID == GetCurrentProcessId() && t.th32ThreadID != me)
            o += thread_stack(t.th32ThreadID, out + o, size - o, returns);
    CloseHandle(snap);
}

static void run(const char *cmd, char *out, int size);
void console_exec(const char *cmd, char *out, int size) { run(cmd, out, size); }

static void run(const char *cmd, char *out, int size)
{
    char arg[300] = "";
    char name[32] = "";
    sscanf(cmd, "%31s %299[^\r\n]", name, arg);
    if (!strcmp(name, "L")) {   /* L <command>: run any console command on the game's logic thread */
        lt_post(arg, out, size);
        return;
    }
    if (probe_is_game_cmd(name)) {   /* these commands touch game state: always on the game thread */
        if (lt_on_game_thread())
            probe_run(cmd, out, size);
        else
            lt_post(cmd, out, size);
        return;
    }
    if (!strcmp(name, "ping")) {
        snprintf(out, size, "pong");
    } else if (!strcmp(name, "version")) {
        snprintf(out, size, "rev1-coop %s pid=%lu exe_ok=%d sha=%s", R1_VERSION, GetCurrentProcessId(), r1_exe_ok(), r1_exe_sha());
    } else if (!strcmp(name, "help")) {
        snprintf(out, size,
                 "ping version base stub frames window shot quit iat threads stacks mainstack readmem <a> [n] readstr <a> [n] dword <a> dump <a> <len> <file> classname <a> dtiname <a> "
                 "objscan <filter|-> <lo> <hi> <file> | pad/padtap/padhold <user> key/keydown/keyup/wmkey mouse mbtn cursor dinput pads steam post log <t> | game thread: L <cmd> "
                 "players [v] units [filter] unit <jill|parker|hex> mode gamepad camera ctl call w32 w8 wf rf probelog ltinfo");
    } else if (!strcmp(name, "base")) {
        BYTE *b = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        int o = snprintf(out, size, "exe base %p size 0x%lx entry +0x%lx\n", b, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.AddressOfEntryPoint);
        IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
            o += snprintf(out + o, size - o, "  %-8.8s %p +0x%lx\n", (char *)s->Name, b + s->VirtualAddress, s->Misc.VirtualSize);
    } else if (!strcmp(name, "stub")) {
        char st[160];
        r1_text_state(st, sizeof st);
        snprintf(out, size, "%s", st);
    } else if (!strcmp(name, "frames")) {
        snprintf(out, size, "%u frames presented", d3d_frames());
    } else if (!strcmp(name, "window")) {
        HWND w = testmode_window();
        RECT r = {0}, c = {0};
        if (!w) {
            snprintf(out, size, "no game window known");
        } else {
            GetWindowRect(w, &r);
            GetClientRect(w, &c);
            snprintf(out, size, "window %p rect %ld,%ld %ldx%ld client %ldx%ld visible=%d iconic=%d foreground=%d style=%08lx", (void *)w, r.left, r.top, r.right - r.left,
                     r.bottom - r.top, c.right, c.bottom, IsWindowVisible(w), IsIconic(w), GetForegroundWindow() == w, (unsigned long)GetWindowLongA(w, GWL_STYLE));
        }
    } else if (!strcmp(name, "pad") || !strcmp(name, "padtap")) {
        /* pad <user 0-3> off | pad <user> <hexbuttons> [lx ly rx ry lt rt]   padtap <user> <hexbuttons> [ms=250]: press, then release
         * buttons: XInput bit mask (up 1 down 2 left 4 right 8 start 10 back 20 ls 40 rs 80 lb 100 rb 200 a 1000 b 2000 x 4000 y 8000) */
        int slot = -1;
        char b[16] = "0";
        int v[6] = {0, 0, 0, 0, 0, 0};
        if (sscanf(arg, "%d %15s %d %d %d %d %d %d", &slot, b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 2 || slot < 0 || slot > 3) {
            snprintf(out, size, "usage: pad <user 0-3> off|<hexbuttons> [lx ly rx ry lt rt] ; padtap <user> <hexbuttons> [ms]");
        } else if (!strcmp(b, "off")) {
            testmode_pad_set(slot, 0, NULL);
            snprintf(out, size, "pad %d: real pad again", slot);
        } else {
            XINPUT_GAMEPAD g;
            memset(&g, 0, sizeof g);
            g.wButtons = (WORD)hex(b);
            if (!strcmp(name, "padtap")) {
                int ms = v[0] > 0 ? v[0] : 250;
                testmode_pad_set(slot, 1, &g);
                Sleep((DWORD)ms);
                memset(&g, 0, sizeof g);
                testmode_pad_set(slot, 1, &g);
                snprintf(out, size, "pad %d: %s pressed for %d ms", slot, b, ms);
            } else {
                g.sThumbLX = (SHORT)v[0], g.sThumbLY = (SHORT)v[1], g.sThumbRX = (SHORT)v[2], g.sThumbRY = (SHORT)v[3];
                g.bLeftTrigger = (BYTE)v[4], g.bRightTrigger = (BYTE)v[5];
                testmode_pad_set(slot, 1, &g);
                snprintf(out, size, "pad %d: buttons %04x set (until 'pad %d off')", slot, g.wButtons, slot);
            }
        }
    } else if (!strcmp(name, "padhold")) {
        /* padhold <user> <hexbuttons> <ms> [lx ly rx ry lt rt]: set now, neutral (still connected) after ms; no blocking */
        int slot = -1, ms = 250;
        char b[16] = "0";
        int v[6] = {0, 0, 0, 0, 0, 0};
        if (sscanf(arg, "%d %15s %d %d %d %d %d %d %d", &slot, b, &ms, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 2 || slot < 0 || slot > 3) {
            snprintf(out, size, "usage: padhold <user 0-3> <hexbuttons> <ms> [lx ly rx ry lt rt]");
        } else {
            XINPUT_GAMEPAD g;
            memset(&g, 0, sizeof g);
            g.wButtons = (WORD)hex(b);
            g.sThumbLX = (SHORT)v[0], g.sThumbLY = (SHORT)v[1], g.sThumbRX = (SHORT)v[2], g.sThumbRY = (SHORT)v[3];
            g.bLeftTrigger = (BYTE)v[4], g.bRightTrigger = (BYTE)v[5];
            testmode_pad_hold(slot, &g, ms);
            snprintf(out, size, "pad %d: buttons %04x for %d ms", slot, g.wButtons, ms);
        }
    } else if (!strcmp(name, "key") || !strcmp(name, "keydown") || !strcmp(name, "keyup")) {
        /* key <name|0xDIK> [ms=150]: press and release by itself; keydown <name>: hold; keyup <name>: release */
        char k[32] = "";
        int ms = 150, dik, vk;
        sscanf(arg, "%31s %d", k, &ms);
        if (!inject_key_lookup(k, &dik, &vk)) {
            snprintf(out, size, "unknown key %s", k);
        } else if (!strcmp(name, "keyup")) {
            inject_key_up(dik);
            snprintf(out, size, "key %s (dik %02x) released", k, dik);
        } else {
            inject_key(dik, !strcmp(name, "keydown") ? 0 : ms);
            snprintf(out, size, "key %s (dik %02x) down%s", k, dik, !strcmp(name, "keydown") ? " until keyup" : "");
        }
    } else if (!strcmp(name, "wmkey")) {   /* wmkey <name> [ms]: WM_KEYDOWN / WM_KEYUP posted to the game window */
        char k[32] = "";
        int ms = 100, dik, vk;
        sscanf(arg, "%31s %d", k, &ms);
        HWND w = testmode_window();
        if (!w || !inject_key_lookup(k, &dik, &vk) || !vk) {
            snprintf(out, size, "no window or unknown key");
        } else {
            LPARAM lp = 1 | ((LPARAM)dik << 16);
            PostMessageA(w, WM_KEYDOWN, (WPARAM)vk, lp);
            Sleep((DWORD)ms);
            PostMessageA(w, WM_KEYUP, (WPARAM)vk, lp | 0xC0000000);
            snprintf(out, size, "WM_KEYDOWN/UP %s vk %02x", k, vk);
        }
    } else if (!strcmp(name, "mouse")) {   /* mouse <dx> <dy> [wheel]: relative movement delivered with the next reads */
        int dx = 0, dy = 0, wh = 0;
        sscanf(arg, "%d %d %d", &dx, &dy, &wh);
        inject_mouse_move(dx, dy, wh);
        snprintf(out, size, "mouse moved %d %d %d", dx, dy, wh);
    } else if (!strcmp(name, "mbtn")) {    /* mbtn <0 left|1 right|2 middle> [ms=120] */
        int n = 0, ms = 120;
        sscanf(arg, "%d %d", &n, &ms);
        inject_mouse_button(n, ms);
        snprintf(out, size, "mouse button %d down for %d ms", n, ms);
    } else if (!strcmp(name, "cursor")) {  /* cursor <x> <y> (client pixels, what GetCursorPos reports for the game) | cursor off */
        int x = 0, y = 0;
        if (!strncmp(arg, "off", 3)) {
            inject_cursor(0, 0, 0);
            snprintf(out, size, "fake cursor off");
        } else {
            sscanf(arg, "%d %d", &x, &y);
            inject_cursor(1, x, y);
            snprintf(out, size, "fake cursor at client %d,%d", x, y);
        }
    } else if (!strcmp(name, "dinput")) {
        testmode_dinput_stats(out, size);
    } else if (!strcmp(name, "steam")) {
        steam_stats(out, size);
    } else if (!strcmp(name, "pads")) {
        testmode_xinput_stats(out, size);
        int o = (int)strlen(out);
        out[o++] = '\n';
        testmode_xinput_sites(out + o, size - o);
    } else if (!strcmp(name, "post")) {   /* post <hexmsg> [hexwparam] [hexlparam] to the game window */
        char m[16] = "0", wp[16] = "0", lp[16] = "0";
        sscanf(arg, "%15s %15s %15s", m, wp, lp);
        HWND w = testmode_window();
        if (w)
            PostMessageA(w, (UINT)hex(m), (WPARAM)hex(wp), (LPARAM)hex(lp));
        snprintf(out, size, w ? "posted %s" : "no game window known", m);
    } else if (!strcmp(name, "mainstack")) {
        HWND w = testmode_window();
        DWORD tid = w ? GetWindowThreadProcessId(w, NULL) : 0;
        if (tid)
            thread_stack(tid, out, size, 1);
        else
            snprintf(out, size, "no game window known");
    } else if (!strcmp(name, "shot")) {
        d3d_request_shot();
        snprintf(out, size, "shot requested (next presented frame)");
    } else if (!strcmp(name, "quit")) {
        HWND w = testmode_window();
        if (w)
            PostMessageA(w, WM_CLOSE, 0, 0);
        snprintf(out, size, w ? "WM_CLOSE posted" : "no game window known");
    } else if (!strcmp(name, "dump")) {   /* dump <hexaddr> <hexlen> <file>: raw bytes of readable memory (unreadable pages as zeros) */
        char a[32], l[32], f[260];
        if (sscanf(arg, "%31s %31s %259[^\r\n]", a, l, f) != 3) {
            snprintf(out, size, "usage: dump <hexaddr> <hexlen> <file>");
        } else {
            unsigned long addr = hex(a), len = hex(l), done = 0, bad = 0;
            FILE *fp = fopen(f, "wb");
            static unsigned char buf[65536];
            if (!fp) {
                snprintf(out, size, "cannot open %s", f);
            } else {
                while (done < len) {
                    unsigned long k = len - done < sizeof buf ? len - done : sizeof buf;
                    if (!r1_safe_read((void *)(addr + done), buf, k)) {
                        for (unsigned long q = 0; q < k; q += 4096) {
                            unsigned long kk = k - q < 4096 ? k - q : 4096;
                            if (!r1_safe_read((void *)(addr + done + q), buf + q, kk))
                                memset(buf + q, 0, kk), bad += kk;
                        }
                    }
                    fwrite(buf, 1, k, fp);
                    done += k;
                }
                fclose(fp);
                snprintf(out, size, "dumped %lu bytes from %lx to %s (%lu unreadable)", done, addr, f, bad);
            }
        }
    } else if (!strcmp(name, "threads")) {
        all_threads(out, size, 0);
    } else if (!strcmp(name, "stacks")) {
        all_threads(out, size, 1);
    } else if (!strcmp(name, "iat")) {
        r1_iat_check("console");
        snprintf(out, size, "see the log");
    } else if (!strcmp(name, "readmem")) {
        char a[32];
        int n = 16;
        sscanf(arg, "%31s %d", a, &n);
        if (n < 1 || n > 256)
            n = 16;
        unsigned char buf[256];
        if (r1_safe_read((void *)hex(a), buf, (unsigned)n))
            hexdump(buf, n, out, size);
        else
            snprintf(out, size, "unreadable %s", a);
    } else if (!strcmp(name, "dword")) {
        DWORD v;
        if (r1_safe_read((void *)hex(arg), &v, 4))
            snprintf(out, size, "%08lx", v);
        else
            snprintf(out, size, "unreadable %s", arg);
    } else if (!strcmp(name, "readstr")) {
        char a[32];
        int n = 100;
        sscanf(arg, "%31s %d", a, &n);
        if (n < 1 || n > 200)
            n = 100;
        char buf[201];
        int i = 0;
        for (; i < n; i++) {
            if (!r1_safe_read((char *)hex(a) + i, &buf[i], 1))
                break;
            if (!buf[i])
                break;
            if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] > 126)
                buf[i] = '.';
        }
        buf[i] = 0;
        snprintf(out, size, i ? "\"%s\"" : "unreadable or empty %s", i ? buf : a);
    } else if (!strcmp(name, "classname")) {   /* classname <hexaddr>: object -> vtable[4] -> DTI -> name : parents */
        classes_name((unsigned)hex(arg), out, size);
    } else if (!strcmp(name, "dtiname")) {
        classes_dtiname((unsigned)hex(arg), out, size);
    } else if (!strcmp(name, "objscan")) {   /* objscan <name filter or -> <lo> <hi> <file (rest of the line, may contain spaces)> */
        char flt[80] = "", f[260] = "", lo[16] = "0", hi[16] = "0";
        if (sscanf(arg, "%79s %15s %15s %259[^\r\n]", flt, lo, hi, f) < 4)
            snprintf(out, size, "usage: objscan <filter|-> <hexlo> <hexhi> <file> (0 0 = everything; a big range crashed Revelations 2: use windows of 128-256 MB)");
        else
            classes_scan(f, !strcmp(flt, "-") ? "" : flt, (unsigned)hex(lo), (unsigned)hex(hi), out, size);
    } else if (!strcmp(name, "log")) {
        r1_log("console: %s", arg);
        snprintf(out, size, "logged");
    } else {
        snprintf(out, size, "unknown command: %s (help)", name);
    }
}

static char port_text[16];

/* The socket is set up on the thread itself: nothing but CreateThread runs under the loader lock. */
static DWORD WINAPI console_thread(LPVOID arg)
{
    (void)arg;
    char buf[512], out[8000];
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(port_text));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        r1_log("console: cannot listen on 127.0.0.1:%s", port_text);
        return 0;
    }
    r1_log("console: listening on 127.0.0.1:%s", port_text);
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            if (WSAGetLastError() == WSAECONNRESET)   /* (an ICMP port-unreachable for an earlier reply) */
                continue;
            return 0;
        }
        buf[n] = 0;
        out[0] = 0;
        run(buf, out, sizeof out);
        sendto(s, out, (int)strlen(out), 0, (struct sockaddr *)&from, fl);
    }
}

void console_install(void)
{
    if (r1_env("REV1_CONSOLE", port_text, sizeof port_text))
        CreateThread(NULL, 0, console_thread, NULL, 0, NULL);
}
