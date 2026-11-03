/*
 * dinput8.dll proxy for Resident Evil Revelations (Steam build 13298487, 32-bit, SteamDRM-wrapped). STAGE 1 SKELETON: not run yet.
 *
 * Placed next to rerev.exe it forwards DirectInput8Create and installs the mod. The exe on disk is never
 * modified and nothing here touches the Steam stub: this DLL is loaded by the exe's own import table (the Windows
 * loader maps it before the stub's entry point runs), we only patch import slots / COM vtables.
 * Unlike Revelations 2 the .text of rerev.exe is NOT encrypted on disk (entropy 6.41, the stub only covers .bind), so game
 * code can be read and patterns matched as soon as the DLL is loaded; still no game code is touched before the first
 * Present (inline hooks: logic-thread hook lt.c, updatePad probe.c). Test modes (window on monitor 2, background, mute, shots,
 * console, synthetic input) are driven by REV1_* environment variables and are all off in a player's game.
 *
 *   REV1_LOG=<file>   the mod's log (default rev1_coop.log next to the exe, rotated at start: rev1_coop.1..4.log;
 *                     a set REV1_LOG is appended to and never rotated)
 *   REV1_ANYEXE=1     install the hooks on an exe that is not the supported build (lab only)
 * Chain rule: a file dinput8_real.dll next to this DLL (a foreign loader such as Ultimate ASI Loader that the
 * installer renamed) is loaded instead of the system dinput8.dll and gets the forwarded calls.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static DirectInput8Create_t real_DirectInput8Create;
static char log_path[MAX_PATH];
static CRITICAL_SECTION log_lock;
static int exe_ok;
static char exe_sha[65];

void r1_log(const char *fmt, ...)
{
    if (!log_path[0])
        return;
    EnterCriticalSection(&log_lock);
    FILE *f = fopen(log_path, "a");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&log_lock);
}

int r1_env(const char *name, char *out, int size)
{
    DWORD n = GetEnvironmentVariableA(name, out, (DWORD)size);
    if (n == 0 || n >= (DWORD)size) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

int r1_exe_ok(void) { return exe_ok; }
const char *r1_exe_sha(void) { return exe_sha; }

int r1_safe_read(const void *addr, void *out, unsigned size)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, size, &got) && got == size;
}

/* ---- SHA-256 (the gate must not depend on bcrypt/advapi32 being loaded while the loader lock is held) ---- */
typedef struct { unsigned h[8]; unsigned long long len; unsigned char buf[64]; unsigned n; } sha_t;
static const unsigned K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha_t *s, const unsigned char *p)
{
    unsigned w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (unsigned)p[4 * i] << 24 | (unsigned)p[4 * i + 1] << 16 | (unsigned)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        unsigned s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        unsigned t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha_init(sha_t *s)
{
    static const unsigned iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0, s->n = 0;
}

static void sha_update(sha_t *s, const unsigned char *p, unsigned long n)
{
    s->len += n;
    while (n) {
        unsigned take = 64 - s->n < n ? 64 - s->n : (unsigned)n;
        if (s->n == 0 && n >= 64) {
            sha_block(s, p);
            p += 64, n -= 64;
            continue;
        }
        memcpy(s->buf + s->n, p, take);
        s->n += take, p += take, n -= take;
        if (s->n == 64)
            sha_block(s, s->buf), s->n = 0;
    }
}

static void sha_final(sha_t *s, char *hex)
{
    unsigned long long bits = s->len * 8;
    unsigned char pad[72] = {0x80};
    unsigned padlen = (s->n < 56 ? 56 : 120) - s->n;
    for (int i = 0; i < 8; i++)
        pad[padlen + i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_update(s, pad, padlen + 8);
    for (int i = 0; i < 8; i++)
        sprintf(hex + 8 * i, "%08x", s->h[i]);
}

static int hash_exe(char *hex)
{
    char path[MAX_PATH];
    static unsigned char buf[1 << 16];
    if (!GetModuleFileNameA(NULL, path, MAX_PATH))
        return 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    sha_t s;
    sha_init(&s);
    DWORD got;
    while (ReadFile(f, buf, sizeof buf, &got, NULL) && got)
        sha_update(&s, buf, got);
    CloseHandle(f);
    sha_final(&s, hex);
    return 1;
}

/* ---- import table patching (with a table of what we patched: the stub/game may rewrite slots) ---- */
static struct { void **slot; void *hook; const char *dll, *func; } patched[64];
static int n_patched;

void *r1_patch_iat(const char *dll, const char *func, void *hook)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll) != 0)
            continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        void **slots = (void **)(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; i++) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {   /* "#2" asks for ordinal 2 (xinput1_3 is imported by ordinal) */
                if (func[0] != '#' || (unsigned)atoi(func + 1) != IMAGE_ORDINAL(names[i].u1.Ordinal))
                    continue;
            } else {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
                if (func[0] == '#' || strcmp((const char *)ibn->Name, func) != 0)
                    continue;
            }
            DWORD old;
            if (!VirtualProtect(&slots[i], sizeof(void *), PAGE_READWRITE, &old))
                return NULL;
            void *orig = slots[i];
            slots[i] = hook;
            VirtualProtect(&slots[i], sizeof(void *), old, &old);
            if (n_patched < 64)
                patched[n_patched].slot = &slots[i], patched[n_patched].hook = hook, patched[n_patched].dll = dll,
                patched[n_patched++].func = func;
            return orig;
        }
    }
    r1_log("import %s!%s not found in the exe", dll, func);
    return NULL;
}

void r1_iat_check(const char *when)
{
    int ok = 0, changed = 0;
    for (int i = 0; i < n_patched; i++) {
        if (*patched[i].slot == patched[i].hook) {
            ok++;
            continue;
        }
        changed++;
        r1_log("iat(%s): %s!%s slot changed to %p (hook %p)", when, patched[i].dll, patched[i].func, *patched[i].slot,
               patched[i].hook);
    }
    r1_log("iat(%s): %d of %d patched import slots intact, %d changed", when, ok, n_patched, changed);
}

void *r1_patch_vtable(void *obj, int index, void *hook)
{
    void **vt = *(void ***)obj;
    DWORD old;
    if (vt[index] == hook || !VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old))
        return NULL;
    void *orig = vt[index];
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void *), old, &old);
    return orig;
}

/* ---- is the stub done? .text is encrypted until the Steam stub ran: measure bytes of a sample in its middle ---- */
static int text_sample(double *zero, double *x8b)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) != 0 || sec->Misc.VirtualSize < 0x20000)
            continue;
        const unsigned char *p = base + sec->VirtualAddress + sec->Misc.VirtualSize / 2;
        static unsigned char copy[65536];
        if (!r1_safe_read(p, copy, sizeof copy))
            return 0;
        unsigned nz = 0, n8 = 0;
        for (unsigned k = 0; k < sizeof copy; k++)
            nz += copy[k] == 0, n8 += copy[k] == 0x8b;
        *zero = nz * 100.0 / sizeof copy, *x8b = n8 * 100.0 / sizeof copy;
        return 1;
    }
    return 0;
}

int r1_text_decrypted(void)
{
    double z, e;
    return text_sample(&z, &e) && z > 3.0 && e > 2.0;   /* code: ~12% zeros, ~6% 0x8B; random bytes: 0.4% each */
}

void r1_text_state(char *out, int size)
{
    double z, e;
    if (text_sample(&z, &e))
        snprintf(out, size, ".text sample: %.1f%% zero bytes, %.1f%% 0x8B -> %s", z, e, z > 3.0 && e > 2.0 ? "code" : "encrypted");
    else
        snprintf(out, size, ".text sample unreadable");
}

/* ---- crash log ---- */
static LONG CALLBACK log_access_violation(EXCEPTION_POINTERS *x)
{
    static LONG seen;
    EXCEPTION_RECORD *r = x->ExceptionRecord;
    CONTEXT *c = x->ContextRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&seen) > 6)
        return EXCEPTION_CONTINUE_SEARCH;
    BYTE *exe = (BYTE *)GetModuleHandleA(NULL);
    r1_log("AccessViolation at %p (exe+%lx) %s %p eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           r->ExceptionAddress, (unsigned long)((BYTE *)r->ExceptionAddress - exe), r->ExceptionInformation[0] ? "write" : "read",
           (void *)r->ExceptionInformation[1], c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD lo = (DWORD)exe, hi = lo + nt->OptionalHeader.SizeOfImage;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    int n = 0;
    for (DWORD *q = (DWORD *)c->Esp; q < (DWORD *)tib->StackBase && n < 24; q++) {
        DWORD a = *q;
        const unsigned char *p = (const unsigned char *)a;
        if (a < lo + 0x1006 || a >= hi)
            continue;
        if (p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
            (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)) {
            r1_log("  [esp+%04x] %08lx (exe+%lx)", (unsigned)((BYTE *)q - (BYTE *)c->Esp), (unsigned long)a, (unsigned long)(a - lo));
            n++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The game's own exits: code and caller in the log, so a start that ends without a word shows where it decided to. */
typedef void (WINAPI *ExitProcess_t)(UINT);
static ExitProcess_t real_ExitProcess;

static void WINAPI hook_ExitProcess(UINT code)
{
    r1_log("ExitProcess(%u) on thread %lu, caller %p (exe+%lx)", code, GetCurrentThreadId(), __builtin_return_address(0),
           (unsigned long)((BYTE *)__builtin_return_address(0) - (BYTE *)GetModuleHandleA(NULL)));
    real_ExitProcess(code);
}

/* ---- log rotation (default log only): rev1_coop.log + .1 .. .4; a previous log under 4 KB is dropped ---- */
#define LOG_KEEP 5
static void log_rotate(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a))
        return;
    if (a.nFileSizeHigh == 0 && a.nFileSizeLow < 4096) {
        DeleteFileA(path);
        return;
    }
    size_t len = strlen(path);
    if (len < 5 || len + 4 >= MAX_PATH)
        return;
    char stem[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    memcpy(stem, path, len - 4);
    stem[len - 4] = 0;
    snprintf(to, sizeof to, "%s.%d.log", stem, LOG_KEEP - 1);
    DeleteFileA(to);
    for (int i = LOG_KEEP - 2; i >= 0; i--) {
        if (i == 0)
            snprintf(from, sizeof from, "%s", path);
        else
            snprintf(from, sizeof from, "%s.%d.log", stem, i);
        snprintf(to, sizeof to, "%s.%d.log", stem, i + 1);
        MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
    }
}

/* The DLL that gets the forwarded call: dinput8_real.dll beside us (chain rule), else the system one. */
static void load_real_dinput8(HINSTANCE self)
{
    char path[MAX_PATH];
    HMODULE h = NULL;
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (n && slash && slash - path + 18 < MAX_PATH) {
        strcpy(slash + 1, "dinput8_real.dll");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            h = LoadLibraryA(path);
            r1_log("chain: dinput8_real.dll %s", h ? "loaded" : "FAILED to load");
        }
    }
    if (!h) {
        UINT m = GetSystemDirectoryA(path, MAX_PATH);
        if (m == 0 || m + 13 >= MAX_PATH)
            return;
        strcpy(path + m, "\\dinput8.dll");
        h = LoadLibraryA(path);
    }
    if (h)
        real_DirectInput8Create = (DirectInput8Create_t)GetProcAddress(h, "DirectInput8Create");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&log_lock);
        if (!r1_env("REV1_LOG", log_path, MAX_PATH)) {
            DWORD n = GetModuleFileNameA(NULL, log_path, MAX_PATH);
            char *slash = strrchr(log_path, '\\');
            if (n && slash && slash - log_path + 16 < MAX_PATH) {
                strcpy(slash + 1, "rev1_coop.log");
                log_rotate(log_path);
            } else
                log_path[0] = 0;
        }
        r1_log("RE Revelations co-op proxy %s loaded (pid %lu), before the Steam stub runs", R1_VERSION, GetCurrentProcessId());
        char st[160];
        r1_text_state(st, sizeof st);
        r1_log("at load: %s", st);
        load_real_dinput8(inst);
        AddVectoredExceptionHandler(1, log_access_violation);
        real_ExitProcess = (ExitProcess_t)r1_patch_iat("KERNEL32.dll", "ExitProcess", (void *)hook_ExitProcess);
        char v[8];
        if (hash_exe(exe_sha))
            exe_ok = strcmp(exe_sha, R1_EXE_SHA256) == 0;
        if (!exe_ok && r1_env("REV1_ANYEXE", v, sizeof v) && v[0] == '1') {
            exe_ok = 1;
            r1_log("gate: REV1_ANYEXE=1, hooks installed on an unsupported exe");
        }
        r1_log("gate: exe sha256 %s -> %s", exe_sha[0] ? exe_sha : "(unreadable)", exe_ok ? "supported build 13298487" : "UNSUPPORTED, forwarding only");
        if (exe_ok) {
            testmode_install();
            d3d_install();
            steam_install();
            console_install();
            mute_install();
        }
    }
    return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    static LONG first;
    if (InterlockedIncrement(&first) == 1) {
        /* The game itself calls this, so its stub is done: nothing in .text is encrypted any more. */
        char st[160];
        r1_text_state(st, sizeof st);
        r1_log("first DirectInput8Create (the game's own start-up code is running): %s", st);
        r1_iat_check("first DirectInput8Create");
    }
    if (!real_DirectInput8Create)
        return E_FAIL;
    HRESULT hr = real_DirectInput8Create(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && exe_ok)
        testmode_wrap_dinput(*out);
    return hr;
}
