#ifndef R1_H
#define R1_H
#include <windows.h>

#define R1_VERSION "0.0.2-stage1-live"

/* The Steam build 13298487 exe (SHA-256 of rerev.exe on disk, lower case). Any other exe: the proxy only forwards DirectInput8Create. */
#define R1_EXE_SHA256 "1573b79eb921b1e7f571e6deb71c39961c7f1940fad6d19283fc3aea7fca48aa"

/* Addresses of build 13298487 (image base 0x400000, no ASLR). Names are ours (notes\engine-map.md, notes\stage1-live.md). */
#define R1_APP_PTR      0x11E0314u   /* [..] = sApp object (window object): +0x12C HWND, +0x20 / +0x2551 "window is active" bytes, +0x2660 exit flag, +0x2657 NCACTIVATE flag */
#define R1_LOOP_HOOK    0x9BA48Du    /* in the logic thread proc 0x9BA3E0: `mov ecx,[renderer]` (8B 0D 9C 1B 1E 01) before the render step 0xB6D4F0 */
#define R1_WNDPROC      0x9BABD0u    /* the main window's window procedure */
#define R1_SPLAYER_PTR  0x1169394u
#define R1_COOPMGR_PTR  0x1169398u
#define R1_GAMESYS_PTR  0x1169108u
#define R1_GAMEPAD_PTR  0x11E0388u
#define R1_UPDATEPAD    0x6AFBD0u    /* uPlayer::updatePad, __fastcall(unit), prologue 55 8B EC 83 E4 F8 */

/* proxy.c: log, environment, import-table and vtable patches, safe reads */
void r1_log(const char *fmt, ...);
int r1_env(const char *name, char *out, int size);
void *r1_patch_iat(const char *dll, const char *func, void *hook);   /* returns the previous pointer; remembered for r1_iat_check. func "#2" = ordinal 2 */
void *r1_patch_vtable(void *obj, int index, void *hook);
int r1_safe_read(const void *addr, void *out, unsigned size);        /* 1 = all bytes read (ReadProcessMemory on ourselves) */
int r1_exe_ok(void);                                                   /* 1 = the exe is the supported build */
const char *r1_exe_sha(void);
void r1_iat_check(const char *when);                                   /* logs and repairs patched import slots the stub/game changed */
/* How "unpacked" .text is: ratio of 0x00 and 0x8B bytes in a sample (code ~ 12% / 6%, an encrypted body ~ 0.4% each). In Rev1 it is code from the start. */
void r1_text_state(char *out, int size);
int r1_text_decrypted(void);

/* testmode.c: window placement, background mode, adapter, input rules for test runs (REV1_* variables) */
void testmode_install(void);
void testmode_wrap_dinput(void *dinput);
int testmode_background(void);
int testmode_adapter_pick(void *d3d, HWND focus, unsigned asked);
int testmode_force_windowed(void);          /* 1 = test instance: never exclusive full screen */
void testmode_window_size(int *w, int *h);  /* wanted client size, 0 when not set */
HWND testmode_window(void);
void testmode_xinput_stats(char *out, int size);
void testmode_pad_set(int slot, int active, const void *xinput_gamepad);   /* synthetic pad for a slot (console "pad") */
void testmode_pad_hold(int slot, const void *xinput_gamepad, int ms);       /* ... released by itself after ms */
void testmode_pad_keep(int user, int on);
void testmode_pad_human(int game_user);
void testmode_pad_pretend(int user, int active, const void *xinput_gamepad);
DWORD testmode_pad_activity(void);
void testmode_dinput_stats(char *out, int size);
void testmode_xinput_sites(char *out, int size);                              /* which XInput users the game polls, from which call sites */

/* inject.c: synthetic keyboard / mouse for test runs */
int inject_key_lookup(const char *name, int *dik, int *vk);
void inject_key(int dik, int ms);
void inject_key_up(int dik);
void inject_mouse_move(int dx, int dy, int wheel);
void inject_mouse_button(int n, int ms);
void inject_mouse_button_up(int n);
void inject_cursor(int on, int x, int y);
int inject_cursor_pos(POINT *p);
int inject_active(void);
void inject_apply_keyboard(unsigned char *state);
void inject_apply_mouse(void *data, unsigned size);
int inject_apply_data(int dev_kind, void *buf, unsigned objsize, unsigned *count, unsigned room);
int inject_vk_down(int vk);
void inject_vk_from_dik(int dik, int *vk);

/* d3d.c: Direct3D 9 creation/device/Present hooks, screenshots (F11, console "shot", REV1_SHOTS) */
void d3d_install(void);
void d3d_request_shot(void);
unsigned d3d_frames(void);

/* steam.c: the game's own Steam calls (log only): init/callback timing, ISteamUserStats achievement calls */
void steam_install(void);
void steam_stats(char *out, int size);

/* lt.c: run code on the game's logic thread (hook in the thread proc), keep-alive for background test mode */
void lt_install(void);
void lt_post(const char *cmd, char *out, int size);
int lt_on_game_thread(void);
unsigned lt_frames(void);
typedef void (*lt_frame_cb)(void);
void lt_add_frame_cb(lt_frame_cb cb);

/* probe.c: stage 1/2 live probes and the stage 3 partner capture; commands run on the game thread */
void probe_run(const char *cmd, char *out, int size);
int probe_is_game_cmd(const char *name);
void probe_install(void);                                              /* inline hooks into the game (after the first Present) */

/* classes.c: class-name walker (vtable[4] -> DTI), object scan */
void classes_name(unsigned obj, char *out, int size);
void classes_dtiname(unsigned dti, char *out, int size);
void classes_scan(const char *file, const char *filter, unsigned lo, unsigned hi, char *out, int size);
int classes_chain(unsigned dti, char *out, int size);
int classes_object_name(unsigned obj, char *out, int size);           /* just the class name of the object, 0 = not an object */

/* console.c: UDP test console on 127.0.0.1:REV1_CONSOLE */
void console_install(void);
void console_exec(const char *cmd, char *out, int size);

/* mute.c: REV1_MUTE=1 keeps this process's audio sessions muted */
void mute_install(void);

#endif
