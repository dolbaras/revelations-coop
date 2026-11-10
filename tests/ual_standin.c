/*
 * tests/ual_test.sh: a stand-in for rerev.exe (never the game) with the imports the mod hooks: KERNEL32 (ExitProcess, CreateThread,
 * WaitForSingleObject, WaitForMultipleObjects, Sleep, SleepEx, GetProcAddress, LoadLibraryA, LoadLibraryW), d3d9!Direct3DCreate9, DINPUT8!DirectInput8Create,
 * XINPUT1_3 by ordinal (#2 #3 #4 #5), steam_api.dll (8 names) and WINMM. main() does what the game's C runtime does at its start
 * (GetSystemTimeAsFileTime, GetStartupInfoW: where an ASI loader writes its slots back), calls Sleep / GetProcAddress / LoadLibraryA / LoadLibraryW through
 * its imports, then DirectInput8Create, where the mod logs which of its import hooks are still in place ("iat(first DirectInput8Create)").
 * The mod's DLL runs here only with REV1_ANYEXE=1 (its gate is the real exe's SHA-256); none of the game's own code is there to hook.
 */
#include <windows.h>
#include <stdio.h>
#include <d3d9.h>
#include <dinput.h>

DWORD WINAPI XInputGetState(DWORD, void *);
DWORD WINAPI XInputSetState(DWORD, void *);
DWORD WINAPI XInputGetCapabilities(DWORD, DWORD, void *);
void WINAPI XInputEnable(BOOL);
int __cdecl SteamAPI_Init(void);
void __cdecl SteamAPI_RunCallbacks(void);
void __cdecl SteamAPI_RegisterCallback(void *, int);
void __cdecl SteamAPI_UnregisterCallback(void *);
void __cdecl SteamAPI_RegisterCallResult(void *, unsigned long long);
void __cdecl SteamAPI_UnregisterCallResult(void *, unsigned long long);
void *__cdecl SteamInternal_CreateInterface(const char *);
void *__cdecl SteamInternal_ContextInit(void *);

static DWORD WINAPI nothing(LPVOID a) { (void)a; return 0; }

int main(void)
{
    /* (imported, not all called: the imports alone give the mod its slots) */
    void *volatile refs[] = {(void *)Direct3DCreate9, (void *)XInputGetState, (void *)XInputSetState, (void *)XInputGetCapabilities,
                             (void *)XInputEnable, (void *)SteamAPI_Init, (void *)SteamAPI_RunCallbacks, (void *)SteamAPI_RegisterCallback,
                             (void *)SteamAPI_UnregisterCallback, (void *)SteamAPI_RegisterCallResult, (void *)SteamAPI_UnregisterCallResult,
                             (void *)SteamInternal_CreateInterface, (void *)SteamInternal_ContextInit, (void *)ExitProcess, (void *)timeGetTime};
    (void)refs;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    STARTUPINFOW si = {sizeof si};
    GetStartupInfoW(&si);
    Sleep(0);
    SleepEx(0, FALSE);
    HANDLE t = CreateThread(NULL, 0, nothing, NULL, 0, NULL);
    if (t) {
        WaitForSingleObject(t, 5000);
        WaitForMultipleObjects(1, &t, TRUE, 0);
        CloseHandle(t);
    }
    HMODULE k = LoadLibraryW(L"kernel32.dll");
    (void)LoadLibraryA("kernel32.dll");
    (void)GetProcAddress(k, "GetTickCount");
    void *di = NULL;
    HRESULT hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, &di, NULL);
    printf("DirectInput8Create hr=%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && di)
        ((IDirectInput8A *)di)->lpVtbl->Release((IDirectInput8A *)di);
    fflush(stdout);
    return 0;
}
