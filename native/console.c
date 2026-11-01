/*
 * Test console (SKELETON, not run yet), REV1_CONSOLE=<port>: UDP on 127.0.0.1 only (never another interface). One text command per datagram, one
 * text datagram back. Runs on its own thread: commands that read game memory use ReadProcessMemory on ourselves
 * (a bad address answers "unreadable" instead of crashing the game); nothing here calls into game code yet.
 *
 *   ping | version | help | base | stub | frames | iat | threads | stacks (every thread: where it is, likely return addresses)
 *   readmem <hexaddr> [len<=256]   hex dump          readstr <hexaddr> [max<=200]   ascii string
 *   dword <hexaddr>               one 32-bit value   classname <hexaddr>   (object -> vtable[4] -> type info +4)
 *   log <text>                    a line in the mod's log
 * Client: tools\r2console.py
 */
#include <winsock2.h>
#include <windows.h>
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

static void run(const char *cmd, char *out, int size)
{
    char arg[300] = "";
    char name[32] = "";
    sscanf(cmd, "%31s %299[^\r\n]", name, arg);
    if (!strcmp(name, "ping")) {
        snprintf(out, size, "pong");
    } else if (!strcmp(name, "version")) {
        snprintf(out, size, "rev1-coop %s pid=%lu exe_ok=%d sha=%s", R1_VERSION, GetCurrentProcessId(), r1_exe_ok(), r1_exe_sha());
    } else if (!strcmp(name, "help")) {
        snprintf(out, size, "ping version base stub readmem <a> [n] readstr <a> [n] dword <a> dump <a> <len> <file> classname <a> dtiname <a> objscan <filter|-> <lo> <hi> <file> threads stacks iat log <t>");
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
    } else if (!strcmp(name, "objscan")) {   /* objscan <name filter or -> <file (rest of the line, may contain spaces)> */
        char flt[80] = "", f[260] = "", lo[16] = "0", hi[16] = "0";
        if (sscanf(arg, "%79s %15s %15s %259[^\r\n]", flt, lo, hi, f) < 4)
            snprintf(out, size, "usage: objscan <filter|-> <hexlo> <hexhi> <file> (0 0 = everything; a big range crashed the game)");
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
