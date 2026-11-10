/*
 * coopsave1_game.c - rev1 live round 15, TASK 5: the co-op game's own save (coopsave1.h has the facts and the rule). The engine side:
 *   - once the game holds its Steam context, the ISteamRemoteStorage vtable slots the save storage uses (0 FileWrite, 1 FileRead, 6 FileDelete, 13 FileExists,
 *     15 GetFileSize) are wrapped in place (as steam.c wraps ISteamUserStats): a call on "DATA0.DAT" while co-op is on works on the local co-op file, every
 *     other call goes to Steam unchanged;
 *   - the title menu's three calls of FUN_008B5D20 (the slot entry lookup) go through a wrapper that answers "empty" while co-op is on and the co-op file does
 *     not exist (the bytes of the function and of the three call sites are checked first; the other 12 callers stay stock: m15c1);
 *   - the per-frame state (net1_session_active, rolebarrier_level_present), its log lines, the console `coopsave status | file | delete (LAB) | force on|off|auto (LAB)`.
 * REV1_COOPSAVE=0 leaves everything stock (the model before the fix).
 * Round 16 (the lead's decisions): the split screen uses the co-op save too (rule 5: menu_split_armed at the title menu or in a level); every co-op write
 * records how this PC played next to the file (rule 6: COOP0.INF, "net" + the role / "split"); the host's title Continue decide (the call at 0x83DCDD, the
 * slot 0 entry Continue loads) of a co-op save recorded as "net" puts the session's roles back to the recorded ones (lk_force_roles) before the save is read.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "r1.h"
#include "net1.h"
#include "coopsave1.h"

#define TC __attribute__((thiscall))
#define CS_SLOT_ENTRY_VA 0x8B5D20u   /* thiscall(sSaveManager, slot) -> entry, ret 4 */
#define CS_SLOT_TABLE_VA 0x8B5CB0u   /* thiscall(sSaveManager, uint *type, slot) -> entry, ret 8 */
static const unsigned char cs_entry_bytes[30] = {0x51, 0x8B, 0x44, 0x24, 0x08, 0x50, 0x8D, 0x54, 0x24, 0x04, 0x52, 0xC7, 0x44, 0x24, 0x08, 0x02,
                                                 0x00, 0x00, 0x00, 0xE8, 0x78, 0xFF, 0xFF, 0xFF, 0x59, 0xC2, 0x04, 0x00, 0xCC, 0xCC};

static cs1_t CS;
static volatile int g_on_flag;          /* CS.active for the storage thread */
static volatile LONG g_rec_now;         /* rule 6: (mode << 4) | (role + 1) of the record a co-op write puts next to the file now (the storage thread reads it) */
static cs1_rec g_rec_last = {CS1_REC_NONE, -1};
static volatile LONG g_rec_writes, g_rec_fails, g_locks, g_lock_keeps;
static int g_last_lock_why = CS1_LKW_NONE, g_last_lock_role = -1;
static char g_rec_path[MAX_PATH + 32];
static int g_enabled = 1, g_inited, g_vt_done, g_entry_done, g_entry_patched;
static char g_dir[MAX_PATH], g_path[MAX_PATH + 32];
static void **g_vt;
static void *g_orig[16];
static volatile LONG g_reads, g_writes, g_dels, g_exists_calls, g_size_calls, g_fails;

typedef unsigned char(TC *fw_t)(void *self, const char *name, const void *data, int size);
typedef int(TC *fr_t)(void *self, const char *name, void *data, int size);
typedef unsigned char(TC *f1_t)(void *self, const char *name);
typedef int(TC *fs_t)(void *self, const char *name);

static int cs_paths(void)
{
    char la[MAX_PATH];
    if (g_path[0]) return 1;
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la);
    if (!n || n >= sizeof la) return 0;
    snprintf(g_dir, sizeof g_dir, "%s\\CAPCOM\\RESIDENT EVIL REVELATIONS\\%s", la, CS1_DIR);
    snprintf(g_path, sizeof g_path, "%s\\%s", g_dir, CS1_FILE);
    snprintf(g_rec_path, sizeof g_rec_path, "%s\\%s", g_dir, CS1_REC_FILE);
    return 1;
}
/* rule 6: the record next to the co-op file */
static int cs_rec_read(cs1_rec *r)
{
    char t[128];
    DWORD rd = 0;
    r->mode = CS1_REC_NONE, r->role = -1;
    if (!cs_paths()) return 0;
    HANDLE h = CreateFileA(g_rec_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, t, sizeof t - 1, &rd, NULL)) rd = 0;
    CloseHandle(h);
    t[rd] = 0;
    return cs1_rec_parse(t, r);
}
static const char *rec_words(const cs1_rec *r)
{
    return r->mode == CS1_REC_SPLIT ? "split screen" : r->mode == CS1_REC_NET ? (r->role == 1 ? "network, this PC played the partner" : "network, this PC played the lead") : "none";
}
/* the storage thread, after a co-op write: the record of how this PC plays now (tmp + move as the save) */
static void cs_rec_write(void)
{
    LONG v = g_rec_now;
    cs1_rec r = {(int)(v >> 4), (int)(v & 15) - 1};
    char t[64], tmp[MAX_PATH + 40];
    int n = cs1_rec_format(t, sizeof t, &r);
    if (!n || !cs_paths()) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_rec_path);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0;
    int ok = h != INVALID_HANDLE_VALUE && WriteFile(h, t, (DWORD)n, &wr, NULL) && wr == (DWORD)n && FlushFileBuffers(h);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    ok = ok && MoveFileExA(tmp, g_rec_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) InterlockedIncrement(&g_rec_fails), DeleteFileA(tmp);
    else InterlockedIncrement(&g_rec_writes);
    r1_log("coopsave: the role record %s: %s (%s)", CS1_REC_FILE, rec_words(&r), ok ? "written" : "FAILED");
}
static int cs_exists(void)
{
    if (!cs_paths()) return 0;
    DWORD a = GetFileAttributesA(g_path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int cs_size(void)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!cs_paths() || !GetFileAttributesExA(g_path, GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    return d.nFileSizeHigh ? 0 : (int)d.nFileSizeLow;
}

/* ---- the wrapped ISteamRemoteStorage slots (the storage worker's thread) */
static unsigned char TC w_write(void *self, const char *name, const void *data, int size)
{
    if (cs1_route(&CS, name, 1, 0) != CS1_COOP) return ((fw_t)g_orig[0])(self, name, data, size);
    InterlockedIncrement(&g_writes);
    if (!cs_paths() || size < 0) return 0;
    CreateDirectoryA(g_dir, NULL);
    char tmp[MAX_PATH + 40];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0;
    int ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data, (DWORD)size, &wr, NULL) && wr == (DWORD)size && FlushFileBuffers(h);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    ok = ok && MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) InterlockedIncrement(&g_fails), DeleteFileA(tmp);
    r1_log("coopsave: the game wrote its save (%d bytes) to the CO-OP file %s: %s (the solo DATA0.DAT not opened)", size, g_path, ok ? "ok" : "FAILED");
    if (ok) cs_rec_write();   /* rule 6 */
    return (unsigned char)ok;
}
static int TC w_read(void *self, const char *name, void *data, int size)
{
    int r = cs1_route(&CS, name, 0, cs_exists());
    if (r == CS1_SEED) {   /* no co-op file yet: the solo file read only (proc_Save's read-modify-write starts the co-op file from it) */
        int n = ((fr_t)g_orig[1])(self, name, data, size);
        r1_log("coopsave: no co-op file yet: the game's read of %s served from the solo save READ ONLY (%d bytes; its next write goes to the co-op file)", CS1_SOLO_NAME, n);
        return n;
    }
    if (r != CS1_COOP) return ((fr_t)g_orig[1])(self, name, data, size);
    InterlockedIncrement(&g_reads);
    if (!cs_paths() || size <= 0) return 0;
    HANDLE h = CreateFileA(g_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD rd = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, data, (DWORD)size, &rd, NULL)) rd = 0;
    CloseHandle(h);
    r1_log("coopsave: the game read its save from the CO-OP file (%lu of %d bytes)", (unsigned long)rd, size);
    return (int)rd;
}
static unsigned char TC w_delete(void *self, const char *name)
{
    if (cs1_route(&CS, name, 1, 0) != CS1_COOP) return ((f1_t)g_orig[6])(self, name);
    InterlockedIncrement(&g_dels);
    int ok = cs_paths() && DeleteFileA(g_path);
    if (g_rec_path[0]) DeleteFileA(g_rec_path);   /* rule 6: no save, no record */
    r1_log("coopsave: the game deleted its save: the CO-OP file %s (%s)", g_path, ok ? "deleted" : "not there");
    return (unsigned char)ok;
}
static unsigned char TC w_exists(void *self, const char *name)
{
    if (cs1_route(&CS, name, 0, cs_exists()) != CS1_COOP) return ((f1_t)g_orig[13])(self, name);
    InterlockedIncrement(&g_exists_calls);
    return (unsigned char)cs_exists();
}
static int TC w_size(void *self, const char *name)
{
    if (cs1_route(&CS, name, 0, cs_exists()) != CS1_COOP) return ((fs_t)g_orig[15])(self, name);
    InterlockedIncrement(&g_size_calls);
    return cs_size();
}

/* rule 3 at the title menu only (m15c1 LIVE, r49: FUN_008B5D20 returning 0 to ALL its callers crashed the guest at START: `AccessViolation at 00833A3F ... read
 * 00000040 eax=00000000`: its caller 0x83382C uses the entry without a NULL check). The title menu's three calls (0x83DA43 / 0x83DA85: the Campaign page's Continue /
 * Load items; 0x83DCDD: the Continue decide) test for NULL: only they are redirected here; every other caller keeps the stock lookup. */
/* rule 6 at the host's title Continue decide (the call at 0x83DCDD: the slot 0 entry it loads with FUN_008B3430 right after): the co-op save's roles back */
#define CS_CONT_DECIDE_RET 0x83DCE2u
static void cs_continue_decide(void)
{
    lk_t *k = net1_link_peek();
    lk_info in;
    memset(&in, 0, sizeof in);
    in.role = -1;
    if (k) lk_get_info(k, &in);
    cs1_rec rec;
    int have = cs_rec_read(&rec), why = 0;
    int first = in.started && in.phase == CP_PHASE_LOADING && in.epoch == 1 && in.gos == 0 && !rolebarrier_level_present();
    int want = cs1_lock_role(g_on_flag, in.host, in.started, first, cs_exists(), have ? &rec : NULL, in.role, &why);
    g_last_lock_why = why, g_last_lock_role = want;
    if (want == CS1_LK_KEEP) {
        if (g_on_flag) {
            InterlockedIncrement(&g_lock_keeps);
            r1_log("coopsave: Continue decide (co-op save on): the roles stay (%s; record %s, this PC plays the %s)", cs1_lkw_name(why), rec_words(&rec),
                   in.role == 1 ? "partner" : in.role == 0 ? "lead" : "?");
        }
        return;
    }
    int r = lk_force_roles(k, want);
    InterlockedIncrement(&g_locks);
    r1_log("coopsave: Continue decide: the co-op save was made with this host as the %s (record %s): the roles of the save are put back before it is read: lk_force_roles(%s) = %d (%s)",
           want ? "partner" : "lead", rec_words(&rec), want ? "partner" : "lead", r, r == 1 ? "changed: the new table goes to the partner before the host's start" : r == 0 ? "already so" : "REFUSED");
}
static unsigned TC cs_slot_entry(void *sm, unsigned slot)
{
    if (g_on_flag && cs1_slots_hidden(&CS, cs_exists())) return 0;
    unsigned e = ((unsigned(TC *)(void *, unsigned))(uintptr_t)CS_SLOT_ENTRY_VA)(sm, slot);
    if (e && (uintptr_t)__builtin_return_address(0) == CS_CONT_DECIDE_RET) cs_continue_decide();   /* rule 6 */
    return e;
}
#define CS_NSITES 8
/* (m15c3 LIVE: with only the first three the host's New Game still asked "Autosave data will be lost. Continue?" (FUN_0083A000's own slot 0 check, cursor
 * on No): its five calls (0x83A0B9 / 0x83A15F / 0x83A2DC / 0x83A54C / 0x83A5C8) test NULL too and are redirected as well) */
static const unsigned cs_sites[CS_NSITES] = {0x83DA43u, 0x83DA85u, 0x83DCDDu, 0x83A0B9u, 0x83A15Fu, 0x83A2DCu, 0x83A54Cu, 0x83A5C8u};
static int cs_site_ok(unsigned va)
{
    unsigned char c[5];
    int32_t rel;
    if (!r1_safe_read((const void *)(uintptr_t)va, c, 5) || c[0] != 0xE8) return 0;
    memcpy(&rel, c + 1, 4);
    return va + 5 + (unsigned)rel == CS_SLOT_ENTRY_VA;
}
static int cs_code_ok(void)
{
    unsigned char c[30];
    if (!r1_safe_read((const void *)(uintptr_t)CS_SLOT_ENTRY_VA, c, sizeof c) || memcmp(c, cs_entry_bytes, sizeof c)) return 0;
    for (int i = 0; i < CS_NSITES; i++)
        if (!cs_site_ok(cs_sites[i])) return 0;
    return 1;
}
static void cs_patch_entry(void)
{
    if (g_entry_done) return;
    g_entry_done = 1;
    if (!cs_code_ok()) {
        r1_log("coopsave: the title menu's slot lookups differ from this build: left stock (a co-op start without a co-op save shows Continue)");
        return;
    }
    for (int i = 0; i < CS_NSITES; i++) {
        unsigned va = cs_sites[i];
        int32_t rel = (int32_t)((uintptr_t)cs_slot_entry - (va + 5));
        DWORD old;
        if (!VirtualProtect((void *)(uintptr_t)va, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
        memcpy((void *)(uintptr_t)(va + 1), &rel, 4);
        VirtualProtect((void *)(uintptr_t)va, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)va, 5);
    }
    g_entry_patched = 1;
    r1_log("coopsave: the title menu's %d slot lookups (the Campaign page 0x83DA43 / 0x83DA85 / 0x83DCDD, its checks in FUN_0083A000) run through the co-op rule (no Continue / Load / autosave warning while co-op is on without a co-op save)", CS_NSITES);
}
static int code_in_module(void *p)
{
    MEMORY_BASIC_INFORMATION mi;
    return p && VirtualQuery(p, &mi, sizeof mi) && mi.Type == MEM_IMAGE && (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
static void cs_patch_storage(void)
{
    void *ctx = steam_context(), *rs = NULL, **vt = NULL;
    if (g_vt_done || !ctx || !r1_safe_read((char *)ctx + 0x24, &rs, 4) || !rs || !r1_safe_read(rs, &vt, 4) || !vt) return;
    g_vt_done = 1;
    void *cur[16];
    if (!r1_safe_read(vt, cur, sizeof cur)) {
        r1_log("coopsave: ISteamRemoteStorage's vtable %p is not readable: the co-op save is OFF (the solo save would be used)", (void *)vt);
        g_enabled = 0;
        return;
    }
    static const int slots[5] = {0, 1, 6, 13, 15};
    void *fns[5] = {(void *)w_write, (void *)w_read, (void *)w_delete, (void *)w_exists, (void *)w_size};
    for (int i = 0; i < 5; i++)
        if (!code_in_module(cur[slots[i]])) {
            r1_log("coopsave: ISteamRemoteStorage slot %d (%p) is not code: the co-op save is OFF", slots[i], cur[slots[i]]);
            g_enabled = 0;
            return;
        }
    for (int i = 0; i < 5; i++) {
        DWORD old;
        g_orig[slots[i]] = cur[slots[i]];
        if (!VirtualProtect(&vt[slots[i]], sizeof(void *), PAGE_READWRITE, &old)) {
            r1_log("coopsave: slot %d not writable: the co-op save is OFF", slots[i]);
            g_enabled = 0;
            return;
        }
        vt[slots[i]] = fns[i];
        VirtualProtect(&vt[slots[i]], sizeof(void *), old, &old);
    }
    g_vt = vt;
    cs_paths();
    r1_log("coopsave: ISteamRemoteStorage %p (vtable %p): FileWrite / FileRead / FileDelete / FileExists / GetFileSize wrapped: in a co-op game the save is %s, the solo %s is never opened",
           rs, (void *)vt, g_path, CS1_SOLO_NAME);
}

static void cs_frame(void)
{
    if (!g_inited) return;
    if (!g_enabled) {
        g_on_flag = 0;
        return;
    }
    cs_patch_storage();
    cs_patch_entry();
    int was = CS.active;
    /* (m15c5: the main title, not just "no level"; m15l9: the title AREA already, its press-start screen reads the save before the menu page is up) */
    int at_title = menu_title_age_ms() < 1500u || rolebarrier_title_area();
    int level = rolebarrier_level_present(), split = cs1_split_now(menu_split_armed(), menu_title_age_ms() < 1500u, level);   /* rule 5 */
    int session = net1_session_active();
    int on = cs1_step2(&CS, session, split, level, at_title) && g_vt != NULL;
    {   /* rule 6: what a co-op write records now */
        lk_t *k = net1_link_peek();
        lk_info in;
        memset(&in, 0, sizeof in);
        in.role = -1;
        if (k && session) lk_get_info(k, &in);
        cs1_rec r = cs1_rec_now(session && in.started, in.role, menu_split_armed(), &g_rec_last);
        if (!on) r.mode = CS1_REC_NONE, r.role = -1;
        if (r.mode != g_rec_last.mode || r.role != g_rec_last.role) r1_log("coopsave: the co-op writes record %s from now on", rec_words(&r));
        g_rec_last = r;
        InterlockedExchange(&g_rec_now, (LONG)((r.mode << 4) | (r.role + 1)));
    }
    if (on != was || (on && !g_on_flag)) {
        if (on)
            r1_log("coopsave: CO-OP SAVE ON (%s): this PC reads and writes %s (%s); the solo %s is not opened until co-op ends",
                   CS.forced >= 0 ? "LAB force" : session ? "the network session started" : split ? "the split screen is armed: a split-screen game is a co-op game" : "a co-op game goes on",
                   g_path, cs_exists() ? "it exists: Continue continues it" : "not there yet: a fresh start, the title shows New Game", CS1_SOLO_NAME);
        else
            r1_log("coopsave: co-op save OFF (back at the title without a session): the solo save %s again", CS1_SOLO_NAME);
    }
    g_on_flag = on;
}

void coopsave_install(void)
{
    char v[16];
    if (g_inited) return;
    cs1_init(&CS);
    g_enabled = !(r1_env("REV1_COOPSAVE", v, sizeof v) && v[0] == '0');
    g_inited = 1;
    lt_add_frame_cb(cs_frame);
    r1_log("coopsave: %s", g_enabled ? "ready (the co-op game's own save: patched once the game holds its Steam context)" : "OFF by REV1_COOPSAVE=0 (a co-op game would write the solo save)");
}

int coopsave_active(void) { return g_on_flag; }
int coopsave_file_exists(void) { return cs_exists(); }
int coopsave_rec_role1(void)
{
    static DWORD at;
    static int v;
    DWORD now = GetTickCount();
    if (at && now - at < 1000) return v;
    at = now | 1;
    cs1_rec r;
    v = g_enabled && cs_exists() && cs_rec_read(&r) && r.mode == CS1_REC_NET ? r.role + 1 : 0;
    return v;
}

void coopsave_cmd(const char *arg, char *out, int size)
{
    char w[16] = "", w2[16] = "";
    sscanf(arg ? arg : "", "%15s %15s", w, w2);
    if (!strcmp(w, "delete")) {   /* LAB: the restore of a run / a test */
        int ok = cs_paths() && DeleteFileA(g_path);
        snprintf(out, (size_t)size, "coopsave: %s %s", g_path, ok ? "deleted" : "not there");
        return;
    }
    if (!strcmp(w, "force")) {   /* LAB */
        CS.forced = !strcmp(w2, "on") ? 1 : !strcmp(w2, "off") ? 0 : -1;
        snprintf(out, (size_t)size, "coopsave: force %d", CS.forced);
        return;
    }
    cs_paths();
    cs1_rec rf;
    int rh = cs_rec_read(&rf);
    int n = snprintf(out, (size_t)size,
             "coopsave: %s; co-op save %s (on %u, off %u); file %s %s (%d bytes); storage wrapped %s, slot rule %s; calls on the co-op file: read %ld write %ld delete %ld exists %ld size %ld "
             "(failed %ld); co-op reads served from the solo file read-only %u; solo-name calls with co-op off %u; slot lookups answered empty %u",
             g_enabled ? "enabled" : "DISABLED (REV1_COOPSAVE=0)", g_on_flag ? "ON" : "off", CS.ons, CS.offs, g_path, cs_exists() ? "exists" : "absent", cs_size(), g_vt ? "yes" : "no",
             g_entry_patched ? "patched" : "not patched", g_reads, g_writes, g_dels, g_exists_calls, g_size_calls, g_fails, CS.seeded, CS.passed, CS.hidden);
    if (n > 0 && n < size)
        snprintf(out + n, (size_t)(size - n), "; split armed %d; role record %s (%s; writes %ld, failed %ld; a write now records %s); Continue role locks %ld, kept %ld (last: %s%s%s)",
                 menu_split_armed(), rh ? rec_words(&rf) : "none", CS1_REC_FILE, g_rec_writes, g_rec_fails, rec_words(&g_rec_last), g_locks, g_lock_keeps, cs1_lkw_name(g_last_lock_why),
                 g_last_lock_role >= 0 ? " -> " : "", g_last_lock_role == 1 ? "partner" : g_last_lock_role == 0 ? "lead" : "");
}
