/* tests/ual_test.sh: a stand-in steam_api.dll with the names of rerev.exe's imports the mod hooks (steam.c steam_install); never called. */
#define E __declspec(dllexport)
E int __cdecl SteamAPI_Init(void) { return 0; }
E void __cdecl SteamAPI_RunCallbacks(void) { }
E void __cdecl SteamAPI_RegisterCallback(void *c, int i) { (void)c; (void)i; }
E void __cdecl SteamAPI_UnregisterCallback(void *c) { (void)c; }
E void __cdecl SteamAPI_RegisterCallResult(void *c, unsigned long long h) { (void)c; (void)h; }
E void __cdecl SteamAPI_UnregisterCallResult(void *c, unsigned long long h) { (void)c; (void)h; }
E void *__cdecl SteamInternal_CreateInterface(const char *v) { (void)v; return 0; }
E void *__cdecl SteamInternal_ContextInit(void *c) { (void)c; return 0; }
