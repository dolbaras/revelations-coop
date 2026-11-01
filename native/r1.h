#ifndef R1_H
#define R1_H
#include <windows.h>

#define R1_VERSION "0.0.1-skeleton"

/* The Steam build 13298487 exe (SHA-256 of rerev.exe on disk, lower case). Any other exe: the proxy only forwards DirectInput8Create. */
#define R1_EXE_SHA256 "1573b79eb921b1e7f571e6deb71c39961c7f1940fad6d19283fc3aea7fca48aa"

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

/* classes.c: class-name walker (vtable[4] -> DTI), object scan */
void classes_name(unsigned obj, char *out, int size);
void classes_dtiname(unsigned dti, char *out, int size);
void classes_scan(const char *file, const char *filter, unsigned lo, unsigned hi, char *out, int size);

/* console.c: UDP test console on 127.0.0.1:REV1_CONSOLE */
void console_install(void);

#endif
