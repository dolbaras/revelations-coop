/*
 * REV1_MUTE=1: this process's audio sessions are muted (Windows' per-application mute, the same switch as the volume mixer).
 * A thread walks every active render device's session list once a second, finds the sessions of our process id (the game
 * opens its sessions late and sometimes more than one) and sets mute. Nothing else is touched; the system volume and other
 * programs' sessions are never read or changed.
 */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include "r1.h"

static void mute_sessions(IMMDeviceEnumerator *en, DWORD pid, int *count)
{
    IMMDeviceCollection *devs = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &devs)))
        return;
    UINT nd = 0;
    IMMDeviceCollection_GetCount(devs, &nd);
    for (UINT d = 0; d < nd; d++) {
        IMMDevice *dev = NULL;
        IAudioSessionManager2 *mgr = NULL;
        IAudioSessionEnumerator *ses = NULL;
        if (FAILED(IMMDeviceCollection_Item(devs, d, &dev)))
            continue;
        if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&mgr)) &&
            SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(mgr, &ses))) {
            int ns = 0;
            IAudioSessionEnumerator_GetCount(ses, &ns);
            for (int i = 0; i < ns; i++) {
                IAudioSessionControl *ctl = NULL;
                IAudioSessionControl2 *ctl2 = NULL;
                if (FAILED(IAudioSessionEnumerator_GetSession(ses, i, &ctl)))
                    continue;
                if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_IAudioSessionControl2, (void **)&ctl2))) {
                    DWORD spid = 0;
                    if (SUCCEEDED(IAudioSessionControl2_GetProcessId(ctl2, &spid)) && spid == pid) {
                        ISimpleAudioVolume *vol = NULL;
                        if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &IID_ISimpleAudioVolume, (void **)&vol))) {
                            BOOL m = FALSE;
                            ISimpleAudioVolume_GetMute(vol, &m);
                            if (!m) {
                                ISimpleAudioVolume_SetMute(vol, TRUE, NULL);
                                (*count)++;
                            }
                            ISimpleAudioVolume_Release(vol);
                        }
                    }
                    IAudioSessionControl2_Release(ctl2);
                }
                IAudioSessionControl_Release(ctl);
            }
        }
        if (ses)
            IAudioSessionEnumerator_Release(ses);
        if (mgr)
            IAudioSessionManager2_Release(mgr);
        IMMDevice_Release(dev);
    }
    IMMDeviceCollection_Release(devs);
}

static DWORD WINAPI mute_thread(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en))) {
        r1_log("mute: no device enumerator");
        return 0;
    }
    DWORD pid = GetCurrentProcessId();
    int total = 0;
    for (;;) {
        int n = 0;
        mute_sessions(en, pid, &n);
        if (n) {
            total += n;
            r1_log("mute: %d audio session(s) of this process muted (%d so far)", n, total);
        }
        Sleep(1000);
    }
}

void mute_install(void)
{
    char v[8];
    if (r1_env("REV1_MUTE", v, sizeof v) && v[0] == '1')
        CreateThread(NULL, 0, mute_thread, NULL, 0, NULL);
}
