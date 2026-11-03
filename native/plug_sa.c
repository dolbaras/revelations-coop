/*
 * Standalone front end of plug_cap.c: a DLL that can be injected into a running test instance that was started with an older main DLL (no "plug load").
 * LoadLibrary (called from the console: call stdcall <LoadLibraryA> <path ptr>) runs DllMain, which installs the hooks and opens a UDP console on
 * 127.0.0.1:28301 with the commands of the plug-in ("cap on|off|status ...", "padfix on|off"). Lab use only (test copy, loopback).
 * The hot-load path through the main DLL is the normal one; this exists so that one launch (and one wait for the hardware lock) is not lost.
 */
#include <winsock2.h>
#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include "plug_cap.c"

static FILE *sa_log_file;
static void sa_log(const char *fmt, ...)
{
    if (!sa_log_file)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(sa_log_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(sa_log_file, fmt, ap);
    va_end(ap);
    fputc(10, sa_log_file);
    fflush(sa_log_file);
}

static int sa_safe_read(const void *a, void *out, unsigned n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), a, out, n, &got) && got == n;
}

static int sa_pad_peek(int user, void *out)
{
    (void)user, (void)out;
    return 0;
}

static unsigned sa_dti(unsigned vt)
{
    unsigned fn;
    unsigned char c[8];
    if (vt < 0x10000 || !sa_safe_read((void *)(vt + 16), &fn, 4) || fn < 0x10000 || !sa_safe_read((void *)fn, c, 6))
        return 0;
    return c[0] == 0xB8 && c[5] == 0xC3 ? *(unsigned *)(c + 1) : 0;
}

static int sa_object_name(unsigned obj, char *out, int size)
{
    unsigned vt, w[2], d;
    char n[84];
    out[0] = 0;
    if (obj < 0x10000 || !sa_safe_read((void *)obj, &vt, 4) || !(d = sa_dti(vt)) || !sa_safe_read((void *)d, w, 8) || w[1] < 0x10000 || !sa_safe_read((void *)w[1], n, 80))
        return 0;
    n[80] = 0;
    snprintf(out, size, "%s", n);
    return out[0] != 0;
}

static unsigned sa_frames(void) { return 0; }

static const r1_api_t sa_api = {1, sa_log, sa_safe_read, sa_pad_peek, sa_object_name, sa_frames};
static r1_plug_t sa_plug;

static DWORD WINAPI sa_thread(LPVOID arg)
{
    (void)arg;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(28301);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        sa_log("sa: cannot listen on 127.0.0.1:28301");
        return 0;
    }
    sa_log("sa: console on 127.0.0.1:28301");
    for (;;) {
        char buf[400], out[3000], name[32] = "", arg2[330] = "";
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0)
            continue;
        buf[n] = 0;
        out[0] = 0;
        sscanf(buf, "%31s %329[^\r\n]", name, arg2);
        if (!strcmp(name, "P")) {   /* "P cap on ..." like the main console */
            char pn[32] = "", pa[300] = "";
            sscanf(arg2, "%31s %299[^\r\n]", pn, pa);
            if (!sa_plug.on_cmd || !sa_plug.on_cmd(pn, pa, out, sizeof out))
                snprintf(out, sizeof out, "unknown plug-in command %s", pn);
        } else if (!strcmp(name, "ping")) {
            snprintf(out, sizeof out, "pong (plug_sa)");
        } else if (!strcmp(name, "unload")) {
            if (sa_plug.on_exit)
                sa_plug.on_exit();
            snprintf(out, sizeof out, "hooks removed (the DLL stays mapped)");
        } else
            snprintf(out, sizeof out, "commands: P cap on|off|status|dz ..., P padfix on|off, ping, unload");
        sendto(s, out, (int)strlen(out), 0, (struct sockaddr *)&from, fl);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        sa_log_file = fopen("r1plug_sa.log", "a");
        if (plug_init(&sa_api, &sa_plug) == 0)
            CreateThread(NULL, 0, sa_thread, NULL, 0, NULL);
        else
            sa_log("sa: plug_init failed");
    }
    return TRUE;
}
