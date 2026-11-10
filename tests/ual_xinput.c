/* tests/ual_test.sh: a stand-in xinput1_3.dll with the functions rerev.exe imports by ordinal (2 XInputGetState, 3 XInputSetState,
 * 4 XInputGetCapabilities, 5 XInputEnable; ual_xinput.def). Never a real pad: no controller connected. */
#include <windows.h>
__declspec(dllexport) DWORD WINAPI fx_GetState(DWORD u, void *s) { (void)u; (void)s; return ERROR_DEVICE_NOT_CONNECTED; }
__declspec(dllexport) DWORD WINAPI fx_SetState(DWORD u, void *v) { (void)u; (void)v; return ERROR_DEVICE_NOT_CONNECTED; }
__declspec(dllexport) DWORD WINAPI fx_GetCapabilities(DWORD u, DWORD f, void *c) { (void)u; (void)f; (void)c; return ERROR_DEVICE_NOT_CONNECTED; }
__declspec(dllexport) void WINAPI fx_Enable(BOOL e) { (void)e; }
