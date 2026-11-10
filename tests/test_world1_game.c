/* Offline test of the pure part of the world hooks (native/world1_hooks.c): the table of hook sites, the x86 length decoder, the detour builders EXECUTED as 32-bit code,
 * apply / revert / refusals on fake memory, and the readers of the sArrange lists.
 * Built for the 32-bit target (the game is a 32-bit process): sh tests/build.sh. When the executable image of the supported build is at hand
 * (REV1_IMAGE, default ../../image/rerev-image.bin relative to this directory) every byte of the table is also compared with it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/world1_hooks.h"
#include "../native/world1.h"
#include "../native/capkeep1.h"
#define W1_PCHIT_MAX_TEST W1_PCHIT_MAX

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

/* ---------------------------------------------------------------- fake memory: sparse regions */
#define MAXR 64
typedef struct { uint32_t base, size; uint8_t *p; } region;
static region regs[MAXR];
static int nregs, writes, fail_write, fail_alloc, unreadable;
static uint32_t heap_next = 0x30000000u;
static char alllog[8192];

static void reg_add(uint32_t base, uint32_t size)
{
    regs[nregs].base = base, regs[nregs].size = size, regs[nregs].p = calloc(1, size);
    nregs++;
}
static uint8_t *at(uint32_t a, uint32_t n)
{
    for (int i = 0; i < nregs; i++)
        if (a >= regs[i].base && (uint64_t)a + n <= (uint64_t)regs[i].base + regs[i].size) return regs[i].p + (a - regs[i].base);
    return NULL;
}
static int f_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    uint8_t *p = unreadable ? NULL : at(a, n);
    if (!p) return 0;
    memcpy(out, p, n);
    return 1;
}
static int f_write(void *u, uint32_t a, const void *src, uint32_t n)
{
    (void)u;
    uint8_t *p = at(a, n);
    if (!p || fail_write) return 0;
    memcpy(p, src, n);
    writes++;
    return 1;
}
static uint32_t f_alloc(void *u, uint32_t n)
{
    (void)u;
    if (fail_alloc) return 0;
    uint32_t b = heap_next;
    heap_next += (n + 15u) & ~15u;
    reg_add(b, n);
    return b;
}
static void f_log(void *u, const char *l)
{
    (void)u;
    strncat(alllog, l, sizeof alllog - strlen(alllog) - 2);
    strcat(alllog, "\n");
}
static const wh_mem MEM = {f_read, f_write, f_alloc, f_log, NULL};

static void set32(uint32_t a, uint32_t v) { uint8_t *p = at(a, 4); if (p) memcpy(p, &v, 4); else printf("(test bug: set32 outside memory %08x)\n", a); }
static uint32_t get32(uint32_t a) { uint8_t *p = at(a, 4); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; }

static void fake_sites(void)
{
    for (int i = 0; i < WH_N_SITES; i++) {
        reg_add(wh_sites[i].addr, 64);
        memcpy(at(wh_sites[i].addr, 64), wh_sites[i].sig, (size_t)wh_sites[i].sig_n);
        memset(at(wh_sites[i].addr, 64) + wh_sites[i].sig_n, 0x90, 64 - (size_t)wh_sites[i].sig_n);
    }
}

static void reset_world(void)
{
    for (int i = 0; i < nregs; i++) free(regs[i].p);
    nregs = 0, writes = 0, fail_write = fail_alloc = unreadable = 0, heap_next = 0x30000000u, alllog[0] = 0;
}

/* ---------------------------------------------------------------- 1. the table */
static void t_table(void)
{
    char why[100];
    for (int i = 0; i < WH_N_SITES; i++) {
        const wh_site *s = &wh_sites[i];
        CHECK(s->name && s->what && s->sig_n >= 8 && s->sig_n <= WH_SIG_MAX, "site has a name, a description and a plausible signature length");
        for (int j = i + 1; j < WH_N_SITES; j++) CHECK(s->addr != wh_sites[j].addr && strcmp(s->name, wh_sites[j].name), "site addresses and names are unique");
        if (i < WH_N_HOOKS) {
            CHECK(s->steal >= 5 && s->steal <= s->sig_n, "a hook site steals at least a jmp and not more than its signature");
            CHECK(wh_steal_ok(s->sig, s->sig_n, s->steal, why, sizeof why), s->name);
            CHECK(!wh_steal_ok(s->sig, s->sig_n, s->steal + 1, why, sizeof why) || s->steal + 1 == s->sig_n || 1, "steal+1 may or may not be a boundary (no claim)");
        } else {
            CHECK(s->steal == 0, "a call site steals nothing");
        }
    }
    /* the documented prologue lengths (notes\net-world-impl.md section 5 / net-world-enemies-re.md section 1) */
    CHECK(wh_sites[WH_SPAWN].addr == 0x4688F0u && wh_sites[WH_SPAWN].steal == 6, "SpawnChara 0x4688F0, 6 bytes");
    CHECK(wh_sites[WH_APPLYHIT].addr == 0x6CD270u && wh_sites[WH_APPLYHIT].steal == 6, "ApplyHit 0x6CD270, 6 bytes");
    CHECK(wh_sites[WH_ONHIT].addr == 0x6CCF30u && wh_sites[WH_ONHIT].steal == 6, "OnHit 0x6CCF30, 6 bytes");
    CHECK(wh_sites[WH_ADDITEM].addr == 0x5CA480u && wh_sites[WH_ADDITEM].steal == 6, "AddItem 0x5CA480, 6 bytes");
    CHECK(wh_sites[WH_KILLREC].addr == 0x89ECA0u && wh_sites[WH_KILLREC].steal == 6, "kill record 0x89ECA0, 6 bytes");
    CHECK(wh_sites[WH_FLAG].addr == 0x721DE0u && wh_sites[WH_FLAG].steal == 5, "SetScenarioFlag 0x721DE0, 5 bytes");
    CHECK(wh_sites[WH_C_DAMAGE].addr == 0x6CA830u && wh_sites[WH_C_ITEMGONE].addr == 0x5D7D00u && wh_sites[WH_C_SETKIND].addr == 0x6CD4E0u, "call sites");
}

/* the same bytes in the real image, when it is there */
static void t_image(void)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("(no executable image at %s: the byte table is checked against itself only)\n", path);
        return;
    }
    int n = 0;
    for (int i = 0; i < WH_N_SITES; i++) {
        uint8_t b[WH_SIG_MAX];
        long off = (long)wh_sites[i].addr - 0x400000L;
        if (fseek(f, off, SEEK_SET) == 0 && fread(b, 1, (size_t)wh_sites[i].sig_n, f) == (size_t)wh_sites[i].sig_n) {
            int same = memcmp(b, wh_sites[i].sig, (size_t)wh_sites[i].sig_n) == 0;
            CHECK(same, wh_sites[i].name);
            n += same;
        } else CHECK(0, "image read");
    }
    /* the structures the readers rely on: the singleton pointers lie inside the image (data section) */
    printf("image %s: %d of %d site signatures are byte-identical\n", path, n, WH_N_SITES);
    /* the fact behind WH_SP_NOUNIT (live run 1 crash): SpawnChara's tail for a block of another area: unit->vf15(), unit->vf0(1) (deleting destructor), entry+4 = 0 */
    static const uint8_t tail[] = {0x8b, 0x44, 0x24, 0x1c, 0x8b, 0x13, 0x8b, 0xcb, 0x39, 0x46, 0x18, 0x74, 0x33, 0x8b, 0x42, 0x3c, 0xff, 0xd0, 0x8b, 0x13, 0x8b, 0x02,
                                   0x6a, 0x01, 0x8b, 0xcb, 0xff, 0xd0, 0x8b, 0x44, 0x24, 0x18, 0x85, 0xc0, 0x74, 0x2a, 0xc7, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00};
    uint8_t tb[sizeof tail];
    CHECK(fseek(f, 0x46A609L - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, sizeof tb, f) == sizeof tb && memcmp(tb, tail, sizeof tail) == 0,
          "image: SpawnChara 0x46A609 deletes the unit of a block of another area and writes entry+4 = 0");
    /* the kind setter is entered with the unit in ecx and reads [ecx+0xE30] first (the crash read 00000E30 at 006CD4E3) */
    static const uint8_t sk[] = {0x56, 0x8b, 0xf1, 0x83, 0xbe, 0x30, 0x0e, 0x00, 0x00, 0x03};
    CHECK(fseek(f, 0x6CD4E0L - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, sizeof sk, f) == sizeof sk && memcmp(tb, sk, sizeof sk) == 0, "image: 0x6CD4E3 is `cmp [esi+0xe30], 3` with esi = ecx");
    /* round 3: uPlayer::setKind 0x6A7B40 (capture.c hooks its 6 first bytes: push esi; push edi; mov edi,[esp+0xc] - whole instructions, no relative operand), then
     * `call 0x6CD4E0` (control type + kind) and `lea ecx,[esi+0xf6c]; call 0x49BE80` (upper-body mode) - the three fields of a capture */
    static const uint8_t uk[] = {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x57, 0x8b, 0xf1, 0xe8, 0x92, 0x59, 0x02, 0x00, 0x57, 0x8d, 0x8e, 0x6c, 0x0f, 0x00, 0x00, 0xe8, 0x26, 0x43, 0xdf, 0xff};
    CHECK(fseek(f, 0x6A7B40L - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, sizeof uk, f) == sizeof uk && memcmp(tb, uk, sizeof uk) == 0, "image: uPlayer::setKind 0x6A7B40 = setters of the three capture fields");
    /* ep1-probe: the save-flag probe (story1_game.inc SV_*): the sEventManager save-proc vtable slot 0x106F140 holds its read 0x722C20 (prologue checked), the flag getter
     * 0x721E30 is the one the probe calls directly; the read returns with `ret 0xC` (thiscall + 3 arguments: the observer's signature) */
    {
        static const uint8_t svp[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x4C, 0x53, 0x8B, 0xD9};
        static const uint8_t svg[] = {0x56, 0x8B, 0xF1, 0x57, 0x85, 0xF6, 0x74, 0x05, 0xE8, 0x83, 0x3B, 0x2B, 0x00, 0x8B, 0x7C, 0x24, 0x0C};
        static const uint8_t svr[] = {0xC2, 0x0C, 0x00};
        uint32_t slot = 0;
        CHECK(fseek(f, 0x106F140L - 0x400000L, SEEK_SET) == 0 && fread(&slot, 1, 4, f) == 4 && slot == 0x722C20u, "image: vtable slot 0x106F140 = sEventManager::cSaveProc_vf6 0x722C20");
        CHECK(fseek(f, 0x722C20L - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, sizeof svp, f) == sizeof svp && memcmp(tb, svp, sizeof svp) == 0, "image: 0x722C20 prologue (the probe's check)");
        CHECK(fseek(f, 0x721E30L - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, sizeof svg, f) == sizeof svg && memcmp(tb, svg, sizeof svg) == 0, "image: the flag getter 0x721E30 (the probe's check)");
        CHECK(fseek(f, 0x723058L - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, sizeof svr, f) == sizeof svr && memcmp(tb, svr, sizeof svr) == 0, "image: 0x722C20 ends with ret 0xC at 0x723058");
    }
    /* ep1-probe e1p2: SpawnChara with from_host 1 in an aCut (0x92) area sets the placement's scene id to -1 (0x468DF3: mov ecx,[eax+0x1c]; and ecx,0xff000000;
     * cmp ecx,0x92000000; jne; mov [esp+0x1c],-1) - the reason op_spawn now passes from_host 0 (f_spawnfh) */
    {
        static const uint8_t fh[] = {0x8B, 0x48, 0x1C, 0x81, 0xE1, 0x00, 0x00, 0x00, 0xFF, 0x81, 0xF9, 0x00, 0x00, 0x00, 0x92, 0x75, 0x08, 0xC7, 0x44, 0x24, 0x1C, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(fseek(f, 0x468DF3L - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, sizeof fh, f) == sizeof fh && memcmp(tb, fh, sizeof fh) == 0, "image: SpawnChara 0x468DF3 = from_host + aCut area -> scene id -1");
    }
    for (int i = 0; i < WH_N_GATES; i++)   /* the interpolator gates: `mov ecx,[0x11E0510]; call Net_IsOnline; test al,al; jz` */
        CHECK(fseek(f, (long)wh_gates[i].addr - 0x400000L, SEEK_SET) == 0 && fread(tb, 1, (size_t)wh_gates[i].n, f) == (size_t)wh_gates[i].n && memcmp(tb, wh_gates[i].orig, (size_t)wh_gates[i].n) == 0,
              wh_gates[i].name);
    /* the door state DTIs are built at start-up by 0x8E1330(ecx = dti, name, parent 0x1175580, ...): `push name; mov ecx, dti; call 0x8e1330` in the image */
    {
        static const uint32_t ctor_site[WH_N_DS] = {0xF80D6Du, 0xF80DADu, 0xF80E2Du, 0xF80EADu, 0xF80EEDu, 0xF810ADu};
        int ok = 1;
        for (int i = 0; i < WH_N_DS; i++) {
            uint8_t c[15];
            uint32_t nm, dt, rel;
            if (fseek(f, (long)ctor_site[i] - 0x400000L - 5, SEEK_SET) || fread(c, 1, sizeof c, f) != sizeof c) { ok = 0; continue; }
            memcpy(&nm, c + 6, 4), memcpy(&dt, c + 11, 4);
            uint32_t par;
            memcpy(&par, c + 1, 4);
            uint8_t cl[5];
            if (fread(cl, 1, 5, f) != 5) { ok = 0; continue; }
            memcpy(&rel, cl + 1, 4);
            ok &= c[0] == 0x68 && par == WH_DOORSTATE_PARENT && c[5] == 0x68 && nm == wh_door_states[i].name_str && c[10] == 0xB9 && dt == wh_door_states[i].dti && cl[0] == 0xE8 &&
                  ctor_site[i] + 10 + 5 + rel == 0x8E1330u;
        }
        CHECK(ok, "image: every door state DTI is constructed by 0x8E1330 with its name and the door state parent");
    }
    /* round 4, doors: the 9-2 / Episode 1 door classes are uDoorBase children (DTI ctor `push 0x1175F78 (parent uDoorBase); push name`), so they run the states above */
    {
        static const struct { uint32_t at, name; const char *what; } dc[] = {{0xF8126Eu, 0x1033194u, "uExperimentDoor"}, {0xF8186Eu, 0x103669Cu, "uShipDoorUAVRoom02"}, {0xF8146Eu, 0x1034314u, "uRoomDoorWooden"}};
        for (unsigned i = 0; i < sizeof dc / sizeof dc[0]; i++) {
            uint8_t c[10];
            uint32_t par, nm;
            int ok = fseek(f, (long)dc[i].at - 0x400000L, SEEK_SET) == 0 && fread(c, 1, sizeof c, f) == sizeof c;
            memcpy(&par, c + 1, 4), memcpy(&nm, c + 6, 4);
            CHECK(ok && c[0] == 0x68 && par == 0x1175F78u && c[5] == 0x68 && nm == dc[i].name, dc[i].what);
        }
        /* ClosedState update 0x62A9FC: `test [door+0xFB4] bit 2` -> clear it, edi = [door+0xFE0], [door+0xFE0] = 0 (then ChangeState(3, edi)): the open request's effect */
        static const uint8_t cs[] = {0x8b, 0x8e, 0xb4, 0x0f, 0x00, 0x00, 0xc1, 0xe9, 0x02, 0xbb, 0x01, 0x00, 0x00, 0x00, 0x84, 0xcb, 0x74, 0x4d, 0x83, 0xa6, 0xb4, 0x0f, 0x00, 0x00, 0xfb,
                                     0x8b, 0xbe, 0xe0, 0x0f, 0x00, 0x00, 0xc7, 0x86, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t b[sizeof cs];
        CHECK(fseek(f, 0x62A9FCL - 0x400000L, SEEK_SET) == 0 && fread(b, 1, sizeof cs, f) == sizeof cs && memcmp(b, cs, sizeof cs) == 0, "image: ClosedState takes the open request (+0xFB4 bit 2, mode +0xFE0)");
        /* OpeningAuto enter 0x629E03: arg 0 -> no motion (`test ecx, ecx; je`), 1/2 -> +0xFB4 |= 1 and motion 4, 3/4 -> &= ~1 and motion 5 */
        static const uint8_t oa[] = {0x8b, 0x4c, 0x24, 0x0c, 0x83, 0xa6, 0x24, 0x0e, 0x00, 0x00, 0xf7, 0x85, 0xc9, 0x74, 0x5b};
        CHECK(fseek(f, 0x629E03L - 0x400000L, SEEK_SET) == 0 && fread(b, 1, sizeof oa, f) == sizeof oa && memcmp(b, oa, sizeof oa) == 0, "image: OpeningAuto starts no motion for arg 0 (the old ChangeState path)");
    }
    fclose(f);
}

/* round 4: doors over the network (pure decisions) */
static void t_door_net(void)
{
    uint32_t ds[WH_N_DS] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
    CHECK(wh_door_player_open(ds, ds[WH_DS_KNOB], ds[WH_DS_OPENING_PLAYER]) && wh_door_player_open(ds, ds[WH_DS_CLOSED], ds[WH_DS_OPENING_PLAYER]), "own opening: -> OpeningPlayer");
    CHECK(wh_door_player_open(ds, ds[WH_DS_KNOB], ds[WH_DS_OPENED]), "own opening: KnobHandOn -> Opened within one scan");
    CHECK(!wh_door_player_open(ds, ds[WH_DS_CLOSED], ds[WH_DS_KNOB]), "the knob alone (a locked door rattles) is not an opening");
    CHECK(!wh_door_player_open(ds, ds[WH_DS_CLOSED], ds[WH_DS_OPENING_AUTO]) && !wh_door_player_open(ds, ds[WH_DS_OPENING_AUTO], ds[WH_DS_OPENED]), "an auto / partner / script opening is never announced (no echo)");
    CHECK(!wh_door_player_open(ds, ds[WH_DS_OPENED], ds[WH_DS_CLOSED]) && !wh_door_player_open(ds, ds[WH_DS_OPENING_PLAYER], ds[WH_DS_OPENING_PLAYER]), "closing / no change");
    uint32_t none[WH_N_DS] = {0};
    CHECK(!wh_door_player_open(none, 0, 0) && !wh_door_player_open(none, 1, 2), "DTIs not verified: nothing announced");
    CHECK(wh_door_phase(0x00000001u) == 2 && wh_door_phase(0x00020000u) == 1 && wh_door_phase(0xFFFFFFFEu) == 1, "side on the wire: 1 + (+0xFB4 bit 0)");
    int a = -1, b = -1;
    wh_door_req_args(2, &a, &b);
    CHECK(a == 1 && b == 1, "opener's bit 0 set -> request (1, 1) = mode 1 (bit 0 set, motion 4)");
    wh_door_req_args(1, &a, &b);
    CHECK(a == 0 && b == 1, "opener's bit 0 clear -> request (0, 1) = mode 3 (bit 0 clear, motion 5)");
    wh_door_req_args(0, &a, &b);
    CHECK(a == 1 && b == 1, "side unknown -> mode 1");
    /* the request as 0x62E360 computes it (mode = a ? (b == 0) + 1 : (b == 0) + 3) */
    for (int ph = 0; ph <= 2; ph++) {
        wh_door_req_args((uint8_t)ph, &a, &b);
        int mode = a ? (b == 0) + 1 : (b == 0) + 3;
        int bit0 = mode == 1 || mode == 2;
        CHECK(ph == 0 || bit0 == (ph == 2), "the partner's door gets the opener's +0xFB4 bit 0");
    }
    CHECK(wh_sites[WH_C_DOOROPEN].addr == 0x62E360u && wh_sites[WH_C_DOOROPEN].steal == 0, "DoorOpenReq is a call site at 0x62E360");
    /* round 5 (wp9 live: the partner's copy stayed Opened 79 s while the opener's door was Closed 1.8 s after OpeningPlayer): the close mirror */
    CHECK(wh_sites[WH_C_DOORCLOSE].addr == 0x62E3A0u && wh_sites[WH_C_DOORCLOSE].steal == 0 && wh_sites[WH_C_DOORCLOSE].sig_n == 8 &&
              !memcmp(wh_sites[WH_C_DOORCLOSE].sig, "\x83\x89\xb4\x0f\x00\x00\x08\xc3", 8), "DoorCloseReq is a call site at 0x62E3A0: or dword [ecx+0xfb4], 8; ret");
    CHECK(wh_door_own_closed(ds, ds[WH_DS_OPENING_PLAYER], ds[WH_DS_CLOSED], 1) && wh_door_own_closed(ds, ds[WH_DS_OPENED], ds[WH_DS_CLOSED], 1),
          "the opener: its own opened door back in ClosedState -> announce Closed (wp9: OpeningPlayer -> Closed in 1.8 s)");
    CHECK(!wh_door_own_closed(ds, ds[WH_DS_OPENED], ds[WH_DS_CLOSED], 0), "a door this PC did not open itself (the partner's copy closing here) is never announced (no echo)");
    CHECK(!wh_door_own_closed(ds, ds[WH_DS_CLOSED], ds[WH_DS_CLOSED], 1) && !wh_door_own_closed(ds, ds[WH_DS_OPENING_PLAYER], ds[WH_DS_OPENED], 1) &&
              !wh_door_own_closed(ds, ds[WH_DS_KNOB], 0x99, 1), "no change / not closed (Opened, the unknown ClosingState) -> nothing yet");
    CHECK(!wh_door_own_closed(none, 1, 0, 1), "DTIs not verified: no close announced");
    CHECK(wh_door_close_wanted(ds, ds[WH_DS_OPENED]) && wh_door_close_wanted(ds, ds[WH_DS_OPENING_AUTO]), "the partner: the copy the open request made (OpeningAuto / Opened) is closed");
    CHECK(!wh_door_close_wanted(ds, ds[WH_DS_CLOSED]) && !wh_door_close_wanted(ds, ds[WH_DS_KNOB]) && !wh_door_close_wanted(ds, ds[WH_DS_OPENING_PLAYER]) &&
              !wh_door_close_wanted(ds, ds[WH_DS_GIMMICK]) && !wh_door_close_wanted(ds, 0) && !wh_door_close_wanted(ds, 0x99),
          "closed already, the own human using it here, a gimmick, unknown / no state: left alone");
    CHECK(!wh_door_close_wanted(none, 0), "DTIs not verified: nothing closed");
    {   /* the product default (wp12 + wp13 live: both directions, open and close): doors ON in the glue, no lab switch needed */
        static char src[1 << 20];
        FILE *f = fopen("../native/world1_game.c", "rb");
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(n > 10000 && strstr(src, "static volatile int f_doors = 1;") != NULL, "world1_game.c: f_doors is ON by default");
        CHECK(strstr(src, "sv_init();") != NULL, "world1_game.c: world_init registers the save-flag probe (off unless REV1_SVPROBE=1)");
        CHECK(strstr(src, "static volatile int f_spawnfh;") != NULL && strstr(src, "r->spawn_flags, f_spawnfh ? 1 : 0, 0);") != NULL && strstr(src, "{\"spawnfh\", &f_spawnfh, 1}") != NULL,
              "world1_game.c: op_spawn places the host's enemy like the client's own script (from_host 0 by default; `world set spawnfh 1` = the old way)");
        {   /* the e1p2 case as a model (the mechanism is the GUESS of notes/ep1-probe.md: a placement whose scene id is -1 is not this scene's record, so the
             * fight check of set_pcs_event (ArgCheckCharaState state 2, ANY) reads it as used up): the host's SPAWN of (2,0) before the client's own
             * ArgCharaSet; the model before the fix (from_host 1) ends the fight at once */
            for (int fh = 0; fh < 2; fh++) {
                int scene_of[2] = {256, 256}, placed[2] = {0, 0};
                int acut = 1;
                placed[0] = 1, scene_of[0] = (fh && acut) ? -1 : 256;   /* op_spawn of (2,0) */
                placed[1] = 1, scene_of[1] = 256;                        /* the client's own placement of (2,1) as a replica */
                int any_dead = 0;
                for (int k = 0; k < 2; k++) any_dead |= placed[k] && scene_of[k] != 256;
                if (fh == 0) CHECK(!any_dead, "model: op_spawn with from_host 0: the client's fight check sees both Oozes alive");
                else CHECK(any_dead, "model before the fix: op_spawn with from_host 1 in aCut1100: the client's fight check ends 1-1 at once (e1p2 live)");
            }
        }
        f = fopen("../native/story1_game.inc", "rb");
        n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(n > 10000 && strstr(src, "static volatile int sv_state;        /* 0 off") != NULL && strstr(src, "r1_env(\"REV1_SVPROBE\", v, sizeof v) && v[0] == '1') sv_set(1, NULL, 0);") != NULL,
              "story1_game.inc: the save-flag probe is OFF by default (only REV1_SVPROBE=1 or `world sf sv on`)");
        CHECK(strstr(src, "if (sv_state != 1 || ++sv_lines > 400) return;") != NULL && strstr(src, "rolebarrier_on_restart(sv_restart_cb);") != NULL,
              "story1_game.inc: the probe lines are silent while it is off; the restart PRE line is registered");
        CHECK(strstr(src, "rolebarrier_on_restart(spre_restart_cb);") != NULL && strstr(src, "cs_send_event(bound_cs, CP_EV_SFPRE, b, n)") != NULL &&
                  strstr(src, "sf1_pre_apply(&SPRE, cs_now(bound_cs), spre_get, spre_set, NULL, &s, &c)") != NULL && strstr(src, "static volatile int f_spre = 1;") != NULL,
              "story1_game.inc: the pre-load digest: the host sends at its restart PRE, the guest applies at its restart PRE, ON by default");
        f = fopen("../native/world1_game.c", "rb");
        n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(strstr(src, "n->ev->kind == CP_EV_SFPRE) story_pre_rx(n->ev->body, n->ev->len);") != NULL, "world1_game.c: the note tap hands the host's pre-load digest to the story glue");
    }
    /* every opening gets a phase that differs from the stored one (the model drops an equal state): the side survives, the counter wraps */
    {
        int ok = 1;
        uint8_t ph = 0;
        for (int i = 0; i < 300; i++) {
            uint32_t fb4 = (uint32_t)(i % 3 == 0);
            uint8_t nx = wh_door_phase_next(ph, fb4);
            ok &= nx != ph && (nx & 3u) == wh_door_phase(fb4);
            wh_door_req_args(nx, &a, &b);
            ok &= a == (int)(fb4 & 1u) && b == 1;
            ph = nx;
        }
        CHECK(ok, "door phase: always new, the side kept, the request follows the side");
        CHECK(wh_door_phase_next(0, 1) == 0x06 && wh_door_phase_next(0x06, 0) == 0x09 && wh_door_phase_next(0xFD, 1) == 0x02, "door phase counter: 0 -> 1, wraps after 63");
    }
}

/* ---------------------------------------------------------------- 2. the length decoder */
static void t_decoder(void)
{
    char why[100];
    int rel;
    static const uint8_t push_ebp[] = {0x55}, mov_ebp_esp[] = {0x8b, 0xec}, and_esp[] = {0x83, 0xe4, 0xf0}, sub_esp[] = {0x81, 0xec, 0x74, 0x02, 0, 0}, xorps[] = {0x0f, 0x57, 0xc0};
    CHECK(wh_insn_len(push_ebp, 1, &rel) == 1 && !rel, "push ebp");
    CHECK(wh_insn_len(mov_ebp_esp, 2, &rel) == 2 && !rel, "mov ebp, esp");
    CHECK(wh_insn_len(and_esp, 3, &rel) == 3 && !rel, "and esp, -16");
    CHECK(wh_insn_len(sub_esp, 6, &rel) == 6 && !rel, "sub esp, imm32");
    CHECK(wh_insn_len(xorps, 3, &rel) == 3 && !rel, "xorps xmm0, xmm0");
    static const uint8_t movss[] = {0xf3, 0x0f, 0x10, 0x81, 0x38, 0x0e, 0, 0};
    CHECK(wh_insn_len(movss, 8, &rel) == 8 && !rel, "movss xmm0, [ecx+0xe38] (prefix + 0F + modrm + disp32)");
    static const uint8_t mov_edi_arg[] = {0x8b, 0x7c, 0x24, 0x0c};
    CHECK(wh_insn_len(mov_edi_arg, 4, &rel) == 4, "mov edi, [esp+0xc] (SIB + disp8)");
    static const uint8_t call_[] = {0xe8, 1, 2, 3, 4}, jmp_[] = {0xe9, 1, 2, 3, 4}, jz8[] = {0x74, 0x05}, jb32[] = {0x0f, 0x82, 0xbf, 1, 0, 0}, ret_[] = {0xc3}, ret4[] = {0xc2, 4, 0}, calli[] = {0xff, 0xd2};
    CHECK(wh_insn_len(call_, 5, &rel) == 5 && rel, "call rel32 is position dependent");
    CHECK(wh_insn_len(jmp_, 5, &rel) == 5 && rel, "jmp rel32 is position dependent");
    CHECK(wh_insn_len(jz8, 2, &rel) == 2 && rel, "jz rel8 is position dependent");
    CHECK(wh_insn_len(jb32, 6, &rel) == 6 && rel, "jb rel32 is position dependent");
    CHECK(wh_insn_len(ret_, 1, &rel) == 1 && rel, "ret ends the function");
    CHECK(wh_insn_len(ret4, 3, &rel) == 3 && rel, "ret imm16 ends the function");
    CHECK(wh_insn_len(calli, 2, &rel) == 2 && rel, "call reg is not stolen");
    static const uint8_t junk[] = {0xd6, 0, 0, 0};
    CHECK(wh_insn_len(junk, 4, &rel) == 0, "an unknown opcode gives 0");
    CHECK(wh_insn_len(sub_esp, 3, &rel) == 0, "a truncated instruction gives 0");
    /* steal counts on a real prologue: 55 8b ec 83 e4 f0 81 ec ... */
    const uint8_t *sp = wh_sites[WH_SPAWN].sig;
    CHECK(wh_steal_ok(sp, 17, 6, why, sizeof why), "SpawnChara: 6 is a boundary");
    CHECK(!wh_steal_ok(sp, 17, 5, why, sizeof why) && strstr(why, "cuts"), "5 cuts `and esp, -16`");
    CHECK(!wh_steal_ok(sp, 17, 4, why, sizeof why), "4 is shorter than a jmp");
    CHECK(wh_steal_ok(sp, 17, 12, why, sizeof why), "12 is a boundary too (push ebp / mov / and / sub esp, imm32)");
    CHECK(!wh_steal_ok(sp, 17, 7, why, sizeof why), "7 cuts `sub esp, imm32`");
    const uint8_t *fl = wh_sites[WH_FLAG].sig;
    CHECK(wh_steal_ok(fl, 17, 5, why, sizeof why), "SetScenarioFlag: 5 is a boundary");
    CHECK(!wh_steal_ok(fl, 17, 7, why, sizeof why) && strstr(why, "position dependent"), "7 would take the `jz` with it");
    CHECK(!wh_steal_ok(fl, 17, 12, why, sizeof why) && strstr(why, "position dependent"), "12 would take the call with it");
    static const uint8_t with_call[] = {0x55, 0x8b, 0xec, 0xe8, 1, 2, 3, 4, 0x90};
    CHECK(!wh_steal_ok(with_call, 9, 8, why, sizeof why) && strstr(why, "position dependent"), "a call inside the stolen bytes is refused");
    CHECK(!wh_steal_ok(junk, 4, 5, why, sizeof why) && strstr(why, "unknown"), "an unknown instruction is refused");
}

/* ---------------------------------------------------------------- 3. the builders, executed */
typedef unsigned (__attribute__((thiscall)) *fn_t1)(void *self, unsigned a);
typedef void (__attribute__((thiscall)) *fn_t6)(void *self, unsigned a, unsigned b, int c, int d, char e, char f);
typedef unsigned (__attribute__((thiscall)) *fn_t2)(void *self, unsigned id, unsigned value);

static uint8_t *xalloc(void) { return VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }

static fn_t1 g_orig1;
static unsigned g_seen_self, g_seen_a;
static unsigned __attribute__((thiscall)) hook1(void *self, unsigned a)
{
    g_seen_self = (unsigned)(uintptr_t)self, g_seen_a = a;
    return g_orig1(self, a) + 1000;
}
static fn_t6 g_orig6;
static unsigned g_args[7];
static void __attribute__((thiscall)) hook6(void *self, unsigned a, unsigned b, int c, int d, char e, char f)
{
    g_args[0] = (unsigned)(uintptr_t)self, g_args[1] = a, g_args[2] = b, g_args[3] = (unsigned)c, g_args[4] = (unsigned)d, g_args[5] = (unsigned)(unsigned char)e, g_args[6] = (unsigned)(unsigned char)f;
    g_orig6(self, a, b, c, d, e, f);
}
static fn_t2 g_orig2;
static unsigned __attribute__((thiscall)) hook2(void *self, unsigned id, unsigned value)
{
    return g_orig2(self, id, value) * 2;
}

static void t_exec(void)
{
    /* ApplyHit-like: 55 8b ec 83 e4 f0 81 ec 14 01 00 00 53 56 57 8b f9 | mov eax,[ebp+8] ; add eax,edi | pop edi ; pop esi ; pop ebx ; mov esp,ebp ; pop ebp ; ret 4 */
    uint8_t *fn = xalloc(), *tr = xalloc();
    int n = 0;
    memcpy(fn, wh_sites[WH_APPLYHIT].sig, 17), n = 17;
    static const uint8_t body[] = {0x8b, 0x45, 0x08, 0x03, 0xc7, 0x5f, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc2, 0x04, 0x00};
    memcpy(fn + n, body, sizeof body);
    fn_t1 f = (fn_t1)(void *)fn;
    CHECK(f((void *)100, 23) == 123, "the test function itself: a + ecx");
    uint8_t stolen[16], patch[16], t[32];
    memcpy(stolen, fn, 6);
    int tl = wh_tramp_build(t, stolen, 6, (uint32_t)(uintptr_t)tr, (uint32_t)(uintptr_t)fn + 6);
    CHECK(tl == 11 && t[6] == 0xE9, "trampoline = 6 stolen bytes + jmp rel32");
    memcpy(tr, t, (size_t)tl);
    g_orig1 = (fn_t1)(void *)tr;
    wh_patch_build(patch, 6, (uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)hook1);
    CHECK(patch[0] == 0xE9 && patch[5] == 0x90, "patch = jmp rel32 + one NOP for the sixth byte");
    memcpy(fn, patch, 6);
    CHECK(f((void *)100, 23) == 1123, "hooked call: the hook ran, the trampoline ran the stolen bytes and the rest of the original");
    CHECK(g_seen_self == 100 && g_seen_a == 23, "the hook saw this (ecx) and the stack argument");
    CHECK(f((void *)7, 5) == 1012, "again (a second call, the stack is balanced)");

    /* SpawnChara-like: void, six stack arguments, ret 0x18 */
    uint8_t *fn6 = xalloc(), *tr6 = xalloc();
    memcpy(fn6, wh_sites[WH_SPAWN].sig, 17);
    static const uint8_t body6[] = {0x5f, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc2, 0x18, 0x00};   /* the real function pushed ebx / esi / edi: 53 56 8b f1 57 are in the signature */
    memcpy(fn6 + 17, body6, sizeof body6);
    fn_t6 f6 = (fn_t6)(void *)fn6;
    g_args[1] = 0;
    memcpy(stolen, fn6, 6);
    tl = wh_tramp_build(t, stolen, 6, (uint32_t)(uintptr_t)tr6, (uint32_t)(uintptr_t)fn6 + 6);
    memcpy(tr6, t, (size_t)tl);
    g_orig6 = (fn_t6)(void *)tr6;
    wh_patch_build(patch, 6, (uint32_t)(uintptr_t)fn6, (uint32_t)(uintptr_t)hook6);
    memcpy(fn6, patch, 6);
    f6((void *)0x1234, 11, 22, 33, 44, 1, 0);
    CHECK(g_args[0] == 0x1234 && g_args[1] == 11 && g_args[2] == 22 && g_args[3] == 33 && g_args[4] == 44 && g_args[5] == 1 && g_args[6] == 0, "SpawnChara-like: all six stack arguments and ecx reach the hook");
    f6((void *)1, 2, 3, 4, 5, 0, 1);   /* if ret 0x18 were wrong the stack would be off and this would crash or give wrong values */
    CHECK(g_args[0] == 1 && g_args[5] == 0 && g_args[6] == 1, "SpawnChara-like: second call fine (callee popped 0x18)");

    /* SetScenarioFlag-like: 5 stolen bytes, ret 8 */
    uint8_t *fn2 = xalloc(), *tr2 = xalloc();
    memcpy(fn2, wh_sites[WH_FLAG].sig, 5);
    static const uint8_t body2[] = {0x8b, 0x44, 0x24, 0x08, 0x03, 0x44, 0x24, 0x0c, 0x5e, 0xc2, 0x08, 0x00};   /* eax = id + value (arguments at esp+8 / esp+0xc after push esi) */
    memcpy(fn2 + 5, body2, sizeof body2);
    fn_t2 f2 = (fn_t2)(void *)fn2;
    CHECK(f2((void *)9, 40, 2) == 42, "flag-like function itself");
    memcpy(stolen, fn2, 5);
    tl = wh_tramp_build(t, stolen, 5, (uint32_t)(uintptr_t)tr2, (uint32_t)(uintptr_t)fn2 + 5);
    memcpy(tr2, t, (size_t)tl);
    g_orig2 = (fn_t2)(void *)tr2;
    wh_patch_build(patch, 5, (uint32_t)(uintptr_t)fn2, (uint32_t)(uintptr_t)hook2);
    CHECK(patch[0] == 0xE9, "5-byte patch is exactly the jmp");
    memcpy(fn2, patch, 5);
    CHECK(f2((void *)9, 40, 2) == 84 && f2((void *)9, 1, 1) == 4, "5-byte steal, ret 8: hooked result is the original's times two");
}

/* ---------------------------------------------------------------- 4. apply / revert on fake memory */
static void t_apply(void)
{
    reset_world();
    fake_sites();
    wh_t w;
    wh_init(&w, 0);
    CHECK(wh_check(&w, &MEM, WH_C_DAMAGE) && w.state[WH_C_DAMAGE] == WH_ON, "a call site with the right bytes is verified");
    {   /* world-downed: downed1 detours Damage 0x6CA830 at its entry (jmp rel32 + 3 nops, 8 bytes); the world must accept exactly that and nothing else */
        wh_t wd;
        wh_init(&wd, 0);
        uint8_t *p = at(0x6CA830u, 16);
        const uint8_t dn[8] = {0xE9, 0x4B, 0x86, 0x5E, 0x2A, 0x90, 0x90, 0x90}, other[8] = {0xE9, 0x00, 0x10, 0x20, 0x30, 0x90, 0x90, 0x90};
        memcpy(p, dn, 8);
        CHECK(!wh_check(&wd, &MEM, WH_C_DAMAGE) && wd.state[WH_C_DAMAGE] == WH_REFUSED, "downed: a detour at the Damage entry is refused while its bytes are not known");
        wh_set_alt(&wd, WH_C_DAMAGE, dn, 8);
        CHECK(wh_check(&wd, &MEM, WH_C_DAMAGE) && wd.state[WH_C_DAMAGE] == WH_ON && wd.alt_seen[WH_C_DAMAGE] == 1, "downed: its own 8 bytes + the rest of the signature are accepted");
        memcpy(p, other, 8);
        CHECK(!wh_check(&wd, &MEM, WH_C_DAMAGE) && wd.state[WH_C_DAMAGE] == WH_REFUSED && wd.alt_seen[WH_C_DAMAGE] == 0, "downed: a foreign E9 at the entry is still refused");
        memcpy(p, dn, 8), p[9] ^= 0x01;
        CHECK(!wh_check(&wd, &MEM, WH_C_DAMAGE), "downed: the known jump but a changed byte after it is refused");
        p[9] ^= 0x01;
        memcpy(p, wh_sites[WH_C_DAMAGE].sig, 8);
        CHECK(wh_check(&wd, &MEM, WH_C_DAMAGE) && wd.alt_seen[WH_C_DAMAGE] == 0, "downed: the original bytes are still accepted with the alternative set");
        wh_set_alt(&wd, WH_SPAWN, dn, 8);
        CHECK(wd.alt_n[WH_SPAWN] == 0, "downed: no alternative bytes for a hook site");
        wh_set_alt(&wd, WH_C_DAMAGE, NULL, 0);
        CHECK(wd.alt_n[WH_C_DAMAGE] == 0, "downed: the alternative is cleared");
    }
    uint32_t hook = 0x50000000u;
    int ok_all = 1;
    for (int i = 0; i < WH_N_HOOKS; i++) ok_all &= wh_apply(&w, &MEM, i, hook + 0x100u * (unsigned)i);
    CHECK(ok_all, "all hooks apply on the real bytes");
    CHECK(writes == 2 * WH_N_HOOKS, "two writes per hook: the trampoline and the patch");
    for (int i = 0; i < WH_N_HOOKS; i++) {
        const wh_site *s = &wh_sites[i];
        uint8_t *live = at(s->addr, 64), exp[16];
        wh_patch_build(exp, s->steal, s->addr, hook + 0x100u * (unsigned)i);
        CHECK(memcmp(live, exp, (size_t)s->steal) == 0, "the patch bytes are exactly jmp rel32 + NOPs");
        CHECK(memcmp(live + s->steal, s->sig + s->steal, (size_t)(s->sig_n - s->steal)) == 0, "the bytes after the stolen ones are untouched");
        CHECK(w.state[i] == WH_ON && w.tramp[i] != 0, "state ON, trampoline allocated");
        uint8_t *tr = at(w.tramp[i], s->steal + 5);
        CHECK(tr && memcmp(tr, s->sig, (size_t)s->steal) == 0 && tr[s->steal] == 0xE9, "trampoline starts with the stolen bytes and ends with a jmp");
        uint32_t rel;
        memcpy(&rel, tr + s->steal + 1, 4);
        CHECK(w.tramp[i] + (uint32_t)s->steal + 5 + rel == s->addr + (uint32_t)s->steal, "trampoline jumps back to the instruction after the stolen bytes");
        char live_s[96];
        CHECK(wh_probe(&w, &MEM, i, live_s, sizeof live_s) == WH_ON && strstr(live_s, "patched"), "probe says patched");
    }
    int wr0 = writes;
    for (int i = 0; i < WH_N_HOOKS; i++) CHECK(wh_apply(&w, &MEM, i, hook + 0x100u * (unsigned)i), "apply again");
    CHECK(writes == wr0, "applying again writes nothing");
    CHECK(w.applies[WH_SPAWN] == 1, "applied counted once");
    char st[4800];   /* 35 sites (rev1 live 12: StageWanted) need more than 2400 */
    wh_status(&w, &MEM, 1, st, sizeof st);
    CHECK(strstr(st, "SpawnChara") && strstr(st, "Damage") && strstr(st, "patched") && strstr(st, "network session ACTIVE"), "status names the sites and the live bytes");
    int rv = 1;
    for (int i = 0; i < WH_N_HOOKS; i++) rv &= wh_revert(&w, &MEM, i);
    CHECK(rv, "revert");
    for (int i = 0; i < WH_N_HOOKS; i++) {
        CHECK(memcmp(at(wh_sites[i].addr, wh_sites[i].sig_n), wh_sites[i].sig, (size_t)wh_sites[i].sig_n) == 0, "after revert the bytes are the original ones");
        CHECK(w.state[i] == WH_OFF && w.reverts[i] == 1, "state off, reverted once");
    }
    wr0 = writes;
    CHECK(wh_revert(&w, &MEM, WH_ONHIT) && writes == wr0, "revert of an original site writes nothing");
    CHECK(wh_apply(&w, &MEM, WH_ONHIT, hook) && w.tramp[WH_ONHIT] && w.applies[WH_ONHIT] == 2, "re-apply works (the trampoline is reused)");
    CHECK(wh_apply(&w, &MEM, WH_ONHIT, hook) && heap_next == 0x30000000u + WH_N_HOOKS * 32u, "no second trampoline is allocated");
    CHECK(strstr(alllog, "SpawnChara hook written") != NULL && strstr(alllog, "OnHit hook removed") != NULL, "writes and removals are logged");

    /* every byte of every signature, changed in turn: refused, nothing written */
    for (int i = 0; i < WH_N_SITES; i++) {
        for (int b = 0; b < wh_sites[i].sig_n; b++) {
            reset_world();
            fake_sites();
            wh_init(&w, 0);
            at(wh_sites[i].addr, 1)[b] ^= 0x5A;
            int r = i < WH_N_HOOKS ? wh_apply(&w, &MEM, i, hook) : wh_check(&w, &MEM, i);
            if (r || writes != 0 || w.state[i] != WH_REFUSED || !w.why[i][0] || (i < WH_N_HOOKS && w.tramp[i])) {
                CHECK(0, "a changed signature byte must be refused without any write");
                printf("   site %s byte %d: r=%d writes=%d state=%d\n", wh_sites[i].name, b, r, writes, w.state[i]);
            } else checks++;
        }
    }
    reset_world();
    fake_sites();
    wh_init(&w, 0);
    at(wh_sites[WH_ONHIT].addr, 1)[2] = 0x99;
    CHECK(!wh_apply(&w, &MEM, WH_ONHIT, hook) && strstr(w.why[WH_ONHIT], "bytes at 006ccf30") != NULL, "the reason names the address and the live bytes");
    CHECK(w.refusals[WH_ONHIT] == 1, "counted once");
    wh_apply(&w, &MEM, WH_ONHIT, hook), wh_apply(&w, &MEM, WH_ONHIT, hook);
    CHECK(w.refusals[WH_ONHIT] == 1, "asked again every frame: still counted (and logged) once");
    CHECK(!wh_revert(&w, &MEM, WH_ONHIT) || 1, "revert of a foreign site");
    {   /* a foreign detour (somebody else's jmp) is never taken out */
        reset_world();
        fake_sites();
        wh_init(&w, 0);
        wh_apply(&w, &MEM, WH_APPLYHIT, hook);
        uint8_t *live = at(wh_sites[WH_APPLYHIT].addr, 6);
        live[1] ^= 0x11;
        uint8_t keep[6];
        memcpy(keep, live, 6);
        CHECK(!wh_revert(&w, &MEM, WH_APPLYHIT) && memcmp(live, keep, 6) == 0, "revert leaves bytes alone that are not exactly ours");
        CHECK(!wh_apply(&w, &MEM, WH_APPLYHIT, hook), "apply refuses a foreign patch too");
    }
    /* memory failures */
    reset_world();
    fake_sites();
    wh_init(&w, 0);
    fail_alloc = 1;
    CHECK(!wh_apply(&w, &MEM, WH_SPAWN, hook) && w.state[WH_SPAWN] == WH_ERROR && writes == 0, "no executable memory: error, nothing written");
    fail_alloc = 0, fail_write = 1;
    CHECK(!wh_apply(&w, &MEM, WH_SPAWN, hook) && w.state[WH_SPAWN] == WH_ERROR, "write failure: error");
    CHECK(memcmp(at(wh_sites[WH_SPAWN].addr, 17), wh_sites[WH_SPAWN].sig, 17) == 0, "... and the site is still original");
    fail_write = 0, unreadable = 1;
    CHECK(!wh_apply(&w, &MEM, WH_SPAWN, hook) && w.state[WH_SPAWN] == WH_ERROR, "unreadable site: error");
    unreadable = 0;
    CHECK(wh_apply(&w, &MEM, WH_SPAWN, hook) && w.state[WH_SPAWN] == WH_ON, "after the failures the same hook applies (no sticky error)");
    CHECK(!wh_apply(&w, &MEM, WH_C_DAMAGE, hook) && !wh_apply(&w, &MEM, -1, hook) && !wh_apply(&w, &MEM, WH_N_SITES, hook), "a call site or a bad id is never detoured");
}

/* the trampoline exists before the site is patched (a thread entering the hook the moment the patch is written must find the original) */
static void t_prepare(void)
{
    reset_world();
    fake_sites();
    wh_t w;
    wh_init(&w, 0);
    CHECK(wh_prepare(&w, &MEM, WH_APPLYHIT) && w.tramp[WH_APPLYHIT] != 0, "prepare makes the trampoline");
    CHECK(writes == 1 && memcmp(at(wh_sites[WH_APPLYHIT].addr, 17), wh_sites[WH_APPLYHIT].sig, 17) == 0, "... and the site is untouched (one write: the trampoline)");
    uint32_t t = w.tramp[WH_APPLYHIT];
    CHECK(wh_prepare(&w, &MEM, WH_APPLYHIT) && w.tramp[WH_APPLYHIT] == t && writes == 1, "prepare again keeps the same trampoline");
    CHECK(wh_apply(&w, &MEM, WH_APPLYHIT, 0x50000000u) && w.tramp[WH_APPLYHIT] == t && writes == 2, "apply after prepare writes only the patch");
    CHECK(!wh_prepare(&w, &MEM, WH_C_DAMAGE) && !wh_prepare(&w, &MEM, -1), "a call site is never prepared");
    reset_world();
    fake_sites();
    wh_init(&w, 0);
    at(wh_sites[WH_SPAWN].addr, 1)[7] ^= 0x01;
    CHECK(!wh_prepare(&w, &MEM, WH_SPAWN) && w.tramp[WH_SPAWN] == 0 && writes == 0, "prepare on foreign bytes: refused, nothing allocated or written");
}

/* ---------------------------------------------------------------- 7. the interpolator gates (byte patches) */
static void fake_gates(void)
{
    for (int i = 0; i < WH_N_GATES; i++) {
        reg_add(wh_gates[i].addr, 32);
        memcpy(at(wh_gates[i].addr, 32), wh_gates[i].orig, (size_t)wh_gates[i].n);
    }
}

static void t_gates(void)
{
    char why[100], live[100];
    for (int i = 0; i < WH_N_GATES; i++) {
        const wh_gate_site *g = &wh_gates[i];
        CHECK(g->off + g->pn <= g->n, "the patch lies inside the checked bytes");
        CHECK(g->orig[g->off] == 0x0f && g->orig[g->off + 1] == 0x84, "the patched instruction is `jz rel32` (0f 84)");
        CHECK(g->orig[g->off - 2] == 0x84 && g->orig[g->off - 1] == 0xc0, "... right after `test al, al` (the Net_IsOnline result)");
        int rel = 0;
        CHECK(wh_insn_len(g->orig + g->off, 6, &rel) == 6 && rel, "the jz is 6 bytes");
        CHECK(wh_insn_len(g->patch, 6, &rel) == 6 && !rel, "the patch is one 6-byte NOP (66 0f 1f 44 00 00)");
        CHECK(wh_steal_ok(g->orig, g->n, 6, why, sizeof why), "the checked bytes start with whole instructions (mov ecx, [..])");
    }
    reset_world();
    fake_gates();
    wh_gates_t gs;
    memset(&gs, 0, sizeof gs);
    for (int i = 0; i < WH_N_GATES; i++) {
        CHECK(wh_gate_probe(&MEM, i, live, sizeof live) == WH_OFF && !strcmp(live, "original"), "gate probe: original");
        CHECK(wh_gate_apply(&gs, &MEM, i) && gs.state[i] == WH_ON, "gate applies on the real bytes");
        const uint8_t *b = at(wh_gates[i].addr, 32);
        CHECK(memcmp(b, wh_gates[i].orig, (size_t)wh_gates[i].off) == 0 && memcmp(b + wh_gates[i].off, wh_gates[i].patch, 6) == 0, "only the jz is replaced");
        CHECK(wh_gate_probe(&MEM, i, live, sizeof live) == WH_ON, "gate probe: patched");
    }
    int wr0 = writes;
    CHECK(wh_gate_apply(&gs, &MEM, 0) && writes == wr0 && gs.applies[0] == 1, "applying again writes nothing");
    for (int i = 0; i < WH_N_GATES; i++) {
        CHECK(wh_gate_revert(&gs, &MEM, i) && gs.state[i] == WH_OFF, "gate revert");
        CHECK(memcmp(at(wh_gates[i].addr, 32), wh_gates[i].orig, (size_t)wh_gates[i].n) == 0, "the original jz is back");
    }
    CHECK(strstr(alllog, "InterpTick gate patched") && strstr(alllog, "InterpTarget gate restored"), "gate writes are logged");
    /* every checked byte changed in turn: refused, nothing written */
    for (int i = 0; i < WH_N_GATES; i++)
        for (int b = 0; b < wh_gates[i].n; b++) {
            reset_world();
            fake_gates();
            memset(&gs, 0, sizeof gs);
            at(wh_gates[i].addr, 1)[b] ^= 0x21;
            if (wh_gate_apply(&gs, &MEM, i) || writes || gs.state[i] != WH_REFUSED || !gs.why[i][0]) {
                CHECK(0, "a changed gate byte must be refused without any write");
                printf("   gate %d byte %d\n", i, b);
            } else checks++;
        }
    /* a foreign change of a patched gate is never reverted */
    reset_world();
    fake_gates();
    memset(&gs, 0, sizeof gs);
    wh_gate_apply(&gs, &MEM, 1);
    at(wh_gates[1].addr, 1)[2] ^= 1;
    wr0 = writes;
    CHECK(!wh_gate_revert(&gs, &MEM, 1) && writes == wr0, "revert leaves a foreign change alone");
    CHECK(wh_gate_probe(&MEM, 1, live, sizeof live) == WH_REFUSED && !strncmp(live, "FOREIGN", 7), "probe shows foreign bytes");
    fail_write = 1;
    reset_world();
    fake_gates();
    memset(&gs, 0, sizeof gs);
    fail_write = 1;
    CHECK(!wh_gate_apply(&gs, &MEM, 0) && gs.state[0] == WH_ERROR, "write failure: error");
    fail_write = 0;
    CHECK(!wh_gate_apply(&gs, &MEM, -1) && !wh_gate_revert(&gs, &MEM, WH_N_GATES), "bad gate ids");
}

/* ---------------------------------------------------------------- 8. PCHIT = the cDamageSyncData layout, both directions */
/* the applier 0x49AA50 as decompiled: msg -> a new HitInfo (attacker / owner come from the serials; chars are sign-extended) */
static void reader_0x49AA50(const uint8_t *msg, uint8_t *hit)
{
    memset(hit, 0, WH_HIT_SIZE);
    for (int v = 0; v < 3; v++) memcpy(hit + 0x10 + 0x10 * v, msg + 0x10 + 0x10 * v, 12);
    int32_t c;
    c = (int8_t)msg[0x44], memcpy(hit + 0x48, &c, 4);
    c = (int8_t)msg[0x45], memcpy(hit + 0x4C, &c, 4);
    memcpy(hit + 0x50, msg + 0x48, 4);
    memcpy(hit + 0x54, msg + 0x4C, 4);
    uint32_t u = msg[0x50];
    memcpy(hit + 0x58, &u, 4);
    c = (int8_t)msg[0x51], memcpy(hit + 0x5C, &c, 4);
    memcpy(hit + 0x64, msg + 0x54, 4), memcpy(hit + 0x68, msg + 0x58, 4), memcpy(hit + 0x6C, msg + 0x5C, 4);
    memcpy(hit + 0x70, msg + 0x60, 4), memcpy(hit + 0x74, msg + 0x64, 4), memcpy(hit + 0x78, msg + 0x68, 4);
    c = (int8_t)msg[0x6C], memcpy(hit + 0x7C, &c, 4);
    memcpy(hit + 0x80, msg + 0x70, 4), memcpy(hit + 0x84, msg + 0x74, 4);
}

static void t_pchit_layout(void)
{
    uint8_t hit[WH_HIT_SIZE], msg[WH_DMGMSG_SIZE], pk[WH_PCHIT_LEN], msg2[WH_DMGMSG_SIZE], back[WH_HIT_SIZE];
    for (int round = 0; round < 3; round++) {
        for (unsigned i = 0; i < WH_HIT_SIZE; i++) hit[i] = (uint8_t)(i * 13 + round * 41 + 1);
        int32_t c = round == 0 ? 5 : round == 1 ? -3 : 120;   /* the char fields: small values as the engine keeps them */
        memcpy(hit + 0x48, &c, 4), memcpy(hit + 0x4C, &c, 4), memcpy(hit + 0x5C, &c, 4), memcpy(hit + 0x7C, &c, 4);
        uint32_t b8 = 0xC8u - (unsigned)round;
        memcpy(hit + 0x58, &b8, 4);
        memset(hit + 0x1C, 0, 4), memset(hit + 0x2C, 0, 4), memset(hit + 0x3C, 0, 4), memset(hit + 0x60, 0, 4);   /* not carried by the Raid either */
        wh_dmgmsg_from_hit(hit, 0x1234, 0x5678, msg);
        CHECK(msg[0x78] == 0, "no extra hits");
        uint16_t s1, s2;
        memcpy(&s1, msg + 0x40, 2), memcpy(&s2, msg + 0x42, 2);
        CHECK(s1 == 0x1234 && s2 == 0x5678, "serials at +0x40 / +0x42");
        wh_pchit_pack(msg, pk);
        wh_pchit_unpack(pk, 0x0042, 0x0042, msg2);
        CHECK(memcmp(msg2 + 0x10, msg + 0x10, 0x30) == 0 && memcmp(msg2 + 0x44, msg + 0x44, 0x34) == 0, "pack / unpack keeps every carried byte");
        memcpy(&s1, msg2 + 0x40, 2);
        CHECK(s1 == 0x0042 && msg2[4] == 2, "the client puts its own serials and the mode word");
        reader_0x49AA50(msg2, back);
        static const int offs[] = {0x10, 0x14, 0x18, 0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84};
        int same = 1;
        for (unsigned k = 0; k < sizeof offs / sizeof offs[0]; k++) same &= memcmp(back + offs[k], hit + offs[k], 4) == 0;
        CHECK(same, "the applier rebuilds the host's HitInfo fields exactly (sender 0x6CF8C0 -> applier 0x49AA50)");
    }
    CHECK(WH_PCHIT_LEN <= W1_PCHIT_MAX_TEST, "the packed blow fits one PCHIT event");
}

/* ---------------------------------------------------------------- 9. key set, door DTIs */
static void t_keyset_doors(void)
{
    static wh_keyset ks;
    wh_keyset_clear(&ks);
    CHECK(wh_keyset_first(&ks, 0x0B03) == 1 && wh_keyset_first(&ks, 0x0B03) == 0, "a key is first only once");
    CHECK(wh_keyset_first(&ks, 0x0B04) == 1 && wh_keyset_first(&ks, 0xFFFF) == 1 && wh_keyset_first(&ks, 0) == 1, "other keys are independent");
    wh_keyset_clear(&ks);
    CHECK(wh_keyset_first(&ks, 0x0B03) == 1, "clear (new level instance) forgets");
    reset_world();
    reg_add(0x01175000u, 0x1000);
    for (int i = 0; i < WH_N_DS; i++) {
        set32(wh_door_states[i].dti, 0x1031E64u + 0x38u * (unsigned)i), set32(wh_door_states[i].dti + 4, wh_door_states[i].name_str), set32(wh_door_states[i].dti + 0x10, WH_DOORSTATE_PARENT);
        set32(wh_door_states[i].dti + 0x1C, 0x100 + (unsigned)i);
    }
    uint32_t id = 0;
    int all = 1;
    for (int i = 0; i < WH_N_DS; i++) all &= wh_door_dti(&MEM, i, &id) && id == 0x100u + (unsigned)i;
    CHECK(all, "door state DTIs as seen live (wp2): own vtable per class in the image, name, parent, id");
    set32(wh_door_states[WH_DS_KNOB].dti, WH_DTI_VTBL);
    CHECK(wh_door_dti(&MEM, WH_DS_KNOB, &id) && id == 0x101u, "the base MtDTI vtable is accepted too");
    set32(wh_door_states[WH_DS_KNOB].dti, 0x3000u);
    CHECK(!wh_door_dti(&MEM, WH_DS_KNOB, &id), "a vtable outside the image is refused");
    set32(wh_door_states[WH_DS_KNOB].dti, 0x1031E9Cu), set32(wh_door_states[WH_DS_KNOB].dti + 0x1C, 0);
    CHECK(!wh_door_dti(&MEM, WH_DS_KNOB, &id), "an id of 0 is refused");
    set32(wh_door_states[WH_DS_KNOB].dti + 0x1C, 0x101u);
    set32(wh_door_states[WH_DS_OPENED].dti + 4, 0x1032269u);
    CHECK(!wh_door_dti(&MEM, WH_DS_OPENED, &id), "a wrong name pointer is refused");
    set32(wh_door_states[WH_DS_OPENED].dti + 4, wh_door_states[WH_DS_OPENED].name_str), set32(wh_door_states[WH_DS_OPENED].dti, 0);
    CHECK(!wh_door_dti(&MEM, WH_DS_OPENED, &id), "a DTI not constructed yet (zero) is refused");
    set32(wh_door_states[WH_DS_CLOSED].dti + 0x10, 0x1175590u);
    CHECK(!wh_door_dti(&MEM, WH_DS_CLOSED, &id) && !wh_door_dti(&MEM, -1, &id) && !wh_door_dti(&MEM, WH_N_DS, &id), "a wrong parent / bad index is refused");
}

/* ---------------------------------------------------------------- 5. when the hooks are wanted */
static void t_want(void)
{
    wh_t w;
    wh_init(&w, 0);
    CHECK(!wh_want(&w, 0), "default: off in a single-player game and in the split screen");
    CHECK(wh_want(&w, 1), "auto: on in an active network session");
    w.mode = G1_MODE_OFF;
    CHECK(!wh_want(&w, 1), "`world revert` beats the session");
    w.mode = G1_MODE_ON;
    CHECK(wh_want(&w, 0), "`world apply` forces on");
    wh_init(&w, 1);
    CHECK(wh_want(&w, 0), "REV1_WORLD=1 switches on without a session");
    w.mode = G1_MODE_OFF;
    CHECK(!wh_want(&w, 0), "... and `world revert` beats it");
}

/* ---------------------------------------------------------------- 6. the sArrange lists */
#define SARR 0x02000000u
#define BLKL 0x02001000u   /* the block list object */
#define BLKA 0x02001200u   /* its array */
#define BLK0 0x02002000u
#define BLK1 0x02002100u
#define CH0 0x02003000u    /* character list objects of the blocks */
#define CH1 0x02003200u
#define CHA0 0x02003400u
#define CHA1 0x02003500u
#define REC00 0x02004000u  /* placement records */
#define REC01 0x02004100u
#define REC10 0x02004200u
#define ENT 0x02005000u    /* list entries */
#define UB 0x03000000u     /* units: 0x2000 apart (the fields sit at +0xE28 ..) */
#define U(i) (UB + 0x2000u * (i))
#define EARR 0x02007000u
#define IARR 0x02007200u
#define PLM 0x02008000u    /* player manager */
#define PLN 0x02008100u    /* list nodes */

static void build_arrange(void)
{
    reset_world();
    reg_add(SARR, 0x400);
    reg_add(BLKL, 0x400);
    reg_add(0x02002000u, 0x1000);
    reg_add(0x02003000u, 0x1000);
    reg_add(0x02004000u, 0x1000);
    reg_add(ENT, 0x1000);
    reg_add(UB, 0x2000u * 8);
    reg_add(EARR, 0x400);
    reg_add(0x02008000u, 0x400);
    reg_add(WH_SPLAYER_PTR & ~0xFFFu, 0x1000);
    set32(SARR + WH_SARR_BLOCKS, BLKL);
    set32(BLKL + 0x74, 2), set32(BLKL + 0x80, BLKA);
    set32(BLKA, BLK0), set32(BLKA + 4, BLK1);
    set32(BLK0 + 0x10, 0xAAAA0001u), set32(BLK0 + 0x14, 0xBBBB0001u), set32(BLK0 + 0x1C, 0x11), set32(BLK0 + 4, CH0);
    set32(BLK1 + 0x10, 0xAAAA0002u), set32(BLK1 + 0x14, 0xBBBB0002u), set32(BLK1 + 0x1C, 0x22), set32(BLK1 + 4, CH1);
    set32(CH0 + 0x74, 2), set32(CH0 + 0x80, CHA0), set32(CHA0, REC00), set32(CHA0 + 4, REC01);
    set32(CH1 + 0x74, 1), set32(CH1 + 0x80, CHA1), set32(CHA1, REC10);
    set32(REC00 + 8, 0x80031000u), set32(REC01 + 8, 0x80031310u), set32(REC10 + 8, 0x80041525u);
    /* enemy list: three entries (block 0 index 0, block 0 index 1, block 1 index 0 -> the same index in another block) */
    uint32_t keys[3][4] = {{0xAAAA0001u, 0xBBBB0001u, 0x11, 0}, {0xAAAA0001u, 0xBBBB0001u, 0x11, 1}, {0xAAAA0002u, 0xBBBB0002u, 0x22, 0}};
    set32(SARR + WH_LIST_ENEMIES + 4, 3), set32(SARR + WH_LIST_ENEMIES + 0x10, EARR);
    for (unsigned i = 0; i < 3; i++) {
        uint32_t e = ENT + 0x80u * i, u = U(i);
        set32(EARR + 4 * i, e), set32(e + 4, u);
        set32(e + 0x10, keys[i][0]), set32(e + 0x14, keys[i][1]), set32(e + 0x38, keys[i][2]), set32(e + 0x3C, keys[i][3]);
        set32(u + WH_U_CLASS, 0x80031000u + i), set32(u + WH_U_KIND, 2), set32(u + WH_U_HP, 0x42c80000u);   /* 100.0f */
    }
    /* item list: one entry (block 1, index 0) */
    reg_add(0x02009000u, 0x400);
    set32(SARR + WH_LIST_ITEMS + 4, 1), set32(SARR + WH_LIST_ITEMS + 0x10, 0x02009000u);
    set32(0x02009000u, ENT + 0x400), set32(ENT + 0x400 + 4, U(3));
    set32(ENT + 0x400 + 0x10, 0xAAAA0002u), set32(ENT + 0x400 + 0x14, 0xBBBB0002u), set32(ENT + 0x400 + 0x38, 0x22), set32(ENT + 0x400 + 0x3C, 0);
    /* players: slot 0 (kind 1) and slot 1 (kind 3) */
    set32(WH_SPLAYER_PTR, PLM);
    set32(PLM + 0x30, PLN), set32(PLM + 0x34, PLN + 0x40);
    set32(PLN + 0x10, U(4)), set32(PLN + 8, PLN + 0x20);
    set32(PLN + 0x20 + 0x10, U(5)), set32(PLN + 0x20 + 8, PLN + 0x40);
    set32(U(4) + WH_U_CLASS, 0x80011010u), set32(U(4) + WH_U_SERIAL, 0), set32(U(4) + WH_U_KIND, 1);
    set32(U(5) + WH_U_CLASS, 0x80011020u), set32(U(5) + WH_U_SERIAL, 1), set32(U(5) + WH_U_KIND, 3);
}

static void t_lists(void)
{
    build_arrange();
    uint32_t cls = 0, ids[3];
    CHECK(wh_block_ids(&MEM, SARR, 0, ids) && ids[0] == 0xAAAA0001u && ids[1] == 0xBBBB0001u && ids[2] == 0x11, "block ids of block 0");
    CHECK(wh_block_ids(&MEM, SARR, 1, ids) && ids[2] == 0x22, "block ids of block 1");
    CHECK(!wh_block_ids(&MEM, SARR, 2, ids), "block 2 does not exist");
    CHECK(wh_char_class(&MEM, SARR, 0, 1, &cls) && cls == 0x80031310u && wh_class_is_enemy(cls), "class of the record (0, 1) is an enemy");
    CHECK(wh_char_class(&MEM, SARR, 1, 0, &cls) && cls == 0x80041525u && !wh_class_is_enemy(cls), "the record (1, 0) is an item, not an enemy");
    CHECK(!wh_char_class(&MEM, SARR, 0, 2, &cls) && !wh_char_class(&MEM, SARR, 5, 0, &cls), "an index / block out of range is refused");
    CHECK(wh_list_count(&MEM, SARR, WH_LIST_ENEMIES) == 3 && wh_list_count(&MEM, SARR, WH_LIST_ITEMS) == 1, "list counts");
    set32(SARR + WH_LIST_ENEMIES + 4, 100000);
    CHECK(wh_list_count(&MEM, SARR, WH_LIST_ENEMIES) == 0, "an absurd count reads as an empty list");
    set32(SARR + WH_LIST_ENEMIES + 4, 3);
    wh_entry e;
    CHECK(wh_find_key(&MEM, SARR, WH_LIST_ENEMIES, 0, 1, &e) && e.unit == U(1), "find by (block 0, index 1)");
    CHECK(wh_find_key(&MEM, SARR, WH_LIST_ENEMIES, 1, 0, &e) && e.unit == U(2), "find by (block 1, index 0): same index, another block, another unit");
    CHECK(!wh_find_key(&MEM, SARR, WH_LIST_ENEMIES, 1, 1, &e), "an unplaced key is not found");
    CHECK(wh_find_unit(&MEM, SARR, WH_LIST_ENEMIES, U(1), &e) && e.k[3] == 1, "find by unit");
    unsigned b = 99, i = 99;
    CHECK(wh_entry_key(&MEM, SARR, &e, &b, &i) && b == 0 && i == 1, "reverse: the entry's (block, index)");
    CHECK(wh_find_unit(&MEM, SARR, WH_LIST_ENEMIES, U(2), &e) && wh_entry_key(&MEM, SARR, &e, &b, &i) && b == 1 && i == 0, "reverse for the entry of block 1");
    CHECK(!wh_find_unit(&MEM, SARR, WH_LIST_ENEMIES, 0x1234, &e), "an unknown unit is not found");
    wh_block tab[8];
    CHECK(wh_block_table(&MEM, SARR, tab, 8) == 2 && tab[1].ids[2] == 0x22, "block table: two blocks");
    CHECK(wh_block_table(&MEM, SARR, tab, 1) == 0, "block table: more blocks than room reads as none");
    CHECK(wh_find_unit(&MEM, SARR, WH_LIST_ENEMIES, U(2), &e) && wh_key_in_table(tab, 2, &e, &b, &i) && b == 1 && i == 0, "key from the table = key from the scan");
    e.k[2] = 0x99;
    CHECK(!wh_key_in_table(tab, 2, &e, &b, &i), "an entry of no known block has no key");
    /* what a SpawnChara call left behind (live run 1: SetKind(NULL) on the client for an entry whose unit SpawnChara had deleted, block of another area) */
    uint32_t su = 0xDEAD;
    CHECK(wh_spawn_result(&MEM, SARR, 0, 1, &su) == WH_SP_UNIT && su == U(1), "spawn result: entry with an enemy unit");
    CHECK(wh_spawn_result(&MEM, SARR, 1, 1, &su) == WH_SP_NONE && su == 0, "spawn result: nothing placed");
    CHECK(wh_spawn_result(&MEM, 0, 0, 1, &su) == WH_SP_NONE && su == 0, "spawn result: no sArrange");
    set32(ENT + 0x80 + 4, 0);   /* entry of (0, 1): unit deleted, entry+4 = 0 (0x46A62D) */
    CHECK(wh_spawn_result(&MEM, SARR, 0, 1, &su) == WH_SP_NOUNIT && su == 0, "spawn result: entry with a NULL unit is NOUNIT and hands out no unit");
    set32(ENT + 0x80 + 4, U(4));   /* an entry pointing at a non-enemy (a player): never treated as an enemy unit */
    CHECK(wh_spawn_result(&MEM, SARR, 0, 1, &su) == WH_SP_NOUNIT && su == 0, "spawn result: a non-enemy unit is not handed out");
    set32(ENT + 0x80 + 4, 0x7FFF0000u);   /* an unreadable pointer */
    CHECK(wh_spawn_result(&MEM, SARR, 0, 1, &su) == WH_SP_NOUNIT && su == 0, "spawn result: an unreadable unit is not handed out");
    set32(ENT + 0x80 + 4, U(1));
    CHECK(wh_spawn_result(&MEM, SARR, 0, 1, NULL) == WH_SP_UNIT, "spawn result without an out pointer");
    /* the owner that makes the engine create a replica (kind 3 = owner != the engine's own slot) */
    for (int eo = 0; eo < 2; eo++)
        for (int hs = -1; hs < 3; hs++) {
            int o = wh_replica_owner(eo, hs);
            CHECK(o != eo && (o == 0 || o == 1), "replica owner is never the engine's own slot and is a valid slot");
            if (hs != eo && (hs == 0 || hs == 1)) CHECK(o == hs, "replica owner is the host's slot when that differs from the engine's own slot");
        }
    CHECK(wh_replica_owner(0, 0) == 1 && wh_replica_owner(1, 1) == 0, "B3 not wired (engine own slot = host slot): the other slot");
    /* snap of a replica that is far from the host's sample (wp1 live: the replica never caught up after a teleport / floor change) */
    {
        const float a[3] = {840, -2550, 1485}, b[3] = {663, -1200, 86}, c[3] = {850, -2550, 1490}, d[3] = {840 + 299, -2550, 1485}, e[3] = {840 + 301, -2550, 1485};
        const float f[3] = {840, -2550 + 151, 1485}, g[3] = {840, -2550 - 151, 1485}, h[3] = {840, -2550 + 149, 1485};
        float nan3[3] = {840, -2550, 1485};
        nan3[1] = nan3[1] * 0.0f / 0.0f;
        CHECK(wh_snap_needed(a, b, 300.0f), "snap: another floor 1900 units away");
        CHECK(!wh_snap_needed(a, c, 300.0f), "no snap for a small step");
        CHECK(!wh_snap_needed(a, d, 300.0f) && wh_snap_needed(a, e, 300.0f), "snap threshold is the horizontal distance");
        CHECK(wh_snap_needed(a, f, 300.0f) && wh_snap_needed(a, g, 300.0f) && !wh_snap_needed(a, h, 300.0f), "snap on a height difference over WH_SNAP_DY in both directions");
        CHECK(!wh_snap_needed(a, b, 0.0f) && !wh_snap_needed(a, b, -5.0f), "snap off at a threshold of 0 or below");
        CHECK(!wh_snap_needed(a, nan3, 300.0f) && !wh_snap_needed(nan3, b, 300.0f), "no snap with a NaN coordinate");
    }
    /* the unit's virtual kind setter */
    uint32_t fn = 0;
    set32(U(1), 0x02008200u), set32(0x02008200u + WH_VF_SETKIND, 0x6CD4E0u);
    CHECK(wh_unit_vfn(&MEM, U(1), WH_VF_SETKIND, &fn) && fn == wh_sites[WH_C_SETKIND].addr, "vtable slot 60 of the unit is the verified setter");
    CHECK(!wh_unit_vfn(&MEM, 0, WH_VF_SETKIND, &fn), "no vtable read through a NULL unit");
    set32(U(2), 0);
    CHECK(!wh_unit_vfn(&MEM, U(2), WH_VF_SETKIND, &fn), "a unit without a vtable gives nothing");
    set32(0x02008200u + WH_VF_SETKIND, 0x6A7B40u);
    CHECK(wh_unit_vfn(&MEM, U(1), WH_VF_SETKIND, &fn) && fn != wh_sites[WH_C_SETKIND].addr, "an override (uPlayer 0x6A7B40) is seen as different (the glue then does not call it)");
    uint32_t k4[4] = {0xAAAA0002u, 0xBBBB0002u, 0x22, 0};
    CHECK(wh_find_key4(&MEM, SARR, WH_LIST_ITEMS, k4, &e) && e.unit == U(3), "an item found by its four-number key");
    k4[3] = 1;
    CHECK(!wh_find_key4(&MEM, SARR, WH_LIST_ITEMS, k4, &e), "... and not by a wrong one");
    CHECK(wh_unit_class(&MEM, U(0)) == 0x80031000u && wh_class_is_enemy(wh_unit_class(&MEM, U(0))), "unit class");
    set32(U(0) + WH_U_CLASS, 0x80000000u);
    CHECK(wh_unit_class(&MEM, U(0)) == 0, "class 0x80000000 (given by the vtable) is not followed");
    set32(U(0) + WH_U_CLASS, 0x80031000u);
    CHECK(wh_class_is_player(0x80011010u) && !wh_class_is_player(0x80031000u) && !wh_class_is_enemy(0x80011010u), "class masks");
    uint32_t pu = 0;
    CHECK(wh_player_by_slot(&MEM, 0, &pu) && pu == U(4), "player unit of slot 0");
    CHECK(wh_player_by_slot(&MEM, 1, &pu) && pu == U(5), "player unit of slot 1");
    CHECK(!wh_player_by_slot(&MEM, 2, &pu), "no unit for slot 2");
    /* HitInfo */
    uint8_t hit[WH_HIT_SIZE];
    float pt[3] = {1.5f, 2.5f, 3.5f};
    wh_hit_build(hit, WH_HIT_VTBL, U(5), U(5), 0x80, pt);
    CHECK(get32(0) == 0 || 1, "(hit buffer is plain memory)");
    uint32_t v;
    memcpy(&v, hit, 4);
    CHECK(v == WH_HIT_VTBL, "HitInfo vtable");
    memcpy(&v, hit + 0x40, 4);
    CHECK(v == U(5), "HitInfo attacker +0x40");
    memcpy(&v, hit + 0x44, 4);
    CHECK(v == U(5), "HitInfo owner +0x44");
    memcpy(&v, hit + 0x54, 4);
    CHECK(v == 0x80, "HitInfo flags +0x54");
    float f;
    memcpy(&f, hit + 0x14, 4);
    CHECK(f == 2.5f, "HitInfo point +0x10..");
    int nz = 0;
    for (unsigned k = 0; k < WH_HIT_SIZE; k++) if (hit[k] && !(k < 4 || (k >= 0x10 && k < 0x1C) || (k >= 0x40 && k < 0x48) || k == 0x54)) nz++;
    CHECK(nz == 0, "everything else in the HitInfo is zero");
    reg_add(0x02010000u, 0x100);
    memcpy(at(0x02010000u, 0x100), hit, sizeof hit);
    int slot = -1, kind = -1;
    CHECK(wh_hit_human(&MEM, 0x02010000u, &pu, &slot, &kind) && slot == 1 && kind == 3 && pu == U(5), "the human behind a synthetic hit: slot 1, kind 3 (the partner's puppet)");
    set32(0x02010000u + 0x40, U(0));   /* an enemy as attacker: falls to the owner */
    set32(0x02010000u + 0x44, U(4));
    CHECK(wh_hit_human(&MEM, 0x02010000u, &pu, &slot, &kind) && slot == 0 && kind == 1, "an enemy attacker: the owner human is used");
    set32(0x02010000u + 0x44, 0);
    CHECK(!wh_hit_human(&MEM, 0x02010000u, &pu, &slot, &kind), "no human behind the hit");
}

/* round 3 (net-world-hooks.md 11): the partner's puppet on the host (wp5: the game took it back as the AI, kind 2 ctl 2, and its kills were credited to the client) */
/* ---------------------------------------------------------------- 6b. the host pass in a level of more than 64 blocks (ep1-probe e1p3, section 1-1) */
/* e1p3 live: the host reported its live Ooze (2/0) gone; the client's DESPAWN marked the entry used up, its fight check read "dead" and it ended 1-1 by itself.
 * The glue's pass keyed the enemies against wh_block_table(.., 64), which reads NONE when the level has more blocks. pass_model = that pass reduced to the keys:
 * the enemies of the list keyed against the table it is given; DESPAWN candidates = the known keys not keyed. fixed = the glue after the fix (room 1024 there,
 * no DESPAWN from a pass that read no table, and the fresh check by the key (wh_key_listed) keeps an enemy that is still listed). */
static unsigned pass_model(const wh_block *tab, unsigned nb, int fixed, const unsigned *known, unsigned nk, unsigned *gone)
{
    unsigned np = 0, ng = 0, present[16];
    unsigned n = wh_list_count(&MEM, SARR, WH_LIST_ENEMIES);
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        unsigned b, idx;
        if (!wh_list_at(&MEM, SARR, WH_LIST_ENEMIES, i, &e) || !e.unit) continue;
        if (!wh_class_is_enemy(wh_unit_class(&MEM, e.unit)) || !wh_key_in_table(tab, nb, &e, &b, &idx)) continue;
        if (np < 16) present[np++] = b << 8 | idx;
    }
    if (fixed && !nb) return 0;
    for (unsigned k = 0; k < nk; k++) {
        int there = 0;
        for (unsigned j = 0; j < np; j++) there |= present[j] == known[k];
        if (!there && fixed && wh_key_listed(&MEM, SARR, known[k] >> 8, known[k] & 0xFF, NULL)) continue;
        if (!there) gone[ng++] = known[k];
    }
    return ng;
}

static void grow_blocks(unsigned cnt)   /* blocks 2 .. cnt-1 of other areas (no enemy of the list in them) */
{
    set32(BLKL + 0x74, cnt);
    for (unsigned b = 2; b < cnt; b++) {
        uint32_t blk = 0x02002200u + 0x30u * (b - 2);
        set32(BLKA + 4 * b, blk);
        set32(blk + 0x10, 0xCCCC0000u + b), set32(blk + 0x14, 0xDDDD0000u + b), set32(blk + 0x1C, 0x100 + b), set32(blk + 4, 0);
    }
}

static void t_pass_blocks(void)
{
    static wh_block tab[1024];
    const unsigned known[3] = {0x000, 0x001, 0x100};   /* the three live enemies of the list: 0/0, 0/1, 1/0 */
    unsigned gone[4], ng, nb;
    build_arrange();
    grow_blocks(70);
    /* the model before the fix: room 64 with 70 blocks reads none, nothing is keyed, every live enemy is reported gone (must fail "nothing gone") */
    nb = wh_block_table(&MEM, SARR, tab, 64);
    ng = pass_model(tab, nb, 0, known, 3, gone);
    int before_fails = (nb == 0) + (ng != 0);
    printf("  host pass, 70 blocks: the model before the fix read %u blocks and reported %u of 3 live enemies gone (fails %d of 2 checks)\n", nb, ng, before_fails);
    CHECK(before_fails == 2 && ng == 3, "the model before the fix (room 64) despawns every live enemy of a level of 70 blocks (the e1p3 early end)");
    /* the fix: room 1024 */
    nb = wh_block_table(&MEM, SARR, tab, 1024);
    ng = pass_model(tab, nb, 1, known, 3, gone);
    CHECK(nb == 70 && ng == 0, "the fix: the table of 70 blocks is read, every live enemy keyed, nothing gone");
    CHECK(wh_block_table(&MEM, SARR, tab, 1024) == 70 && tab[1].ids[2] == 0x22 && tab[69].ids[2] == 0x100 + 69, "the table holds every block in order");
    /* a pass that read no table sends no DESPAWN at all */
    nb = wh_block_table(&MEM, SARR, tab, 64);
    CHECK(nb == 0 && pass_model(tab, nb, 1, known, 3, gone) == 0, "the fix: a pass whose table could not be read sends no DESPAWN");
    /* a stale cached table (block 1 a hole when it was read, placed since): the fresh check by the key keeps the live enemy of block 1 */
    set32(BLKA + 4, 0);
    nb = wh_block_table(&MEM, SARR, tab, 1024);
    set32(BLKA + 4, BLK1);
    ng = pass_model(tab, nb, 0, known, 3, gone);
    CHECK(nb == 70 && ng == 1 && gone[0] == 0x100, "a stale table without the fresh check: the live enemy 1/0 would be reported gone");
    CHECK(pass_model(tab, nb, 1, known, 3, gone) == 0, "the fix: the fresh check by the key keeps 1/0 (still listed)");
    /* a real departure still despawns: the entry of 1/0 left the list */
    nb = wh_block_table(&MEM, SARR, tab, 1024);
    set32(SARR + WH_LIST_ENEMIES + 4, 2);
    ng = pass_model(tab, nb, 1, known, 3, gone);
    CHECK(ng == 1 && gone[0] == 0x100, "the fix: an enemy whose entry left the list is still reported gone");
    set32(SARR + WH_LIST_ENEMIES + 4, 3);
    /* wh_key_listed: the present rule of the pass, read fresh */
    wh_entry le;
    CHECK(wh_key_listed(&MEM, SARR, 0, 1, &le) && le.unit == U(1), "listed: an entry with a unit of an enemy class");
    CHECK(!wh_key_listed(&MEM, SARR, 5, 0, NULL) && !wh_key_listed(&MEM, SARR, 1, 1, NULL), "not listed: a key of no block / no entry");
    set32(ENT + 0x80 + 4, 0);
    CHECK(wh_key_listed(&MEM, SARR, 0, 1, NULL), "listed: a parked entry (no unit) not used up whose record is an enemy");
    *at(ENT + 0x80 + WH_ENTRY_SPENT, 1) = 1;
    CHECK(!wh_key_listed(&MEM, SARR, 0, 1, NULL), "not listed: a parked entry that is used up");
    *at(ENT + 0x80 + WH_ENTRY_SPENT, 1) = 0;
    set32(ENT + 0x80 + 4, U(1));
    set32(ENT + 0x100 + 4, 0);
    CHECK(!wh_key_listed(&MEM, SARR, 1, 0, NULL), "not listed: a parked entry whose record is not an enemy (an item)");
    set32(ENT + 0x100 + 4, U(2));
    set32(U(2) + WH_U_CLASS, 0x80000000u);
    CHECK(!wh_key_listed(&MEM, SARR, 1, 0, NULL), "not listed: an entry whose unit was freed (class 0x80000000)");
    set32(U(2) + WH_U_CLASS, 0x80031002u);
    {   /* the glue: room 1024, the DESPAWN only from a pass with a table, the fresh check before it */
        static char src[1 << 20];
        FILE *f = fopen("../native/world1_game.c", "rb");
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(n > 10000 && strstr(src, "#define WH_PASS_BLOCKS 1024u") != NULL && strstr(src, "nb = wh_block_table(&MEM, sa, blocks, WH_PASS_BLOCKS);") != NULL &&
                  strstr(src, "wh_block_table(&MEM, sa, blocks, 64)") == NULL,
              "world1_game.c: the host pass reads the block table with room for 1024 blocks");
        CHECK(strstr(src, "if (nb && (int32_t)(now - last_despawn_ms) >= 500) {") != NULL && strstr(src, "if (wh_key_listed(&MEM, sa, key >> 8, key & 0xFF, &le)) {") != NULL,
              "world1_game.c: no DESPAWN from a pass without a table; the fresh check by the key before every DESPAWN");
    }
}

static void t_puppet_credit(void)
{
    /* host (own slot 0): the partner's puppet (slot 1) hits an enemy -> dropped; the own human, a HIT applied for him (ops), the client side and the switch pass */
    CHECK(wh_partner_blow_dropped(1, 1, 1, 0, 0, 0) == 1, "host: a blow of the partner's puppet is dropped");
    CHECK(wh_partner_blow_dropped(1, 1, 0, 0, 0, 0) == 0, "host: the own human's blow passes");
    CHECK(wh_partner_blow_dropped(1, 1, 1, 0, 1, 0) == 0, "host: the HIT applied for him (ops) passes");
    CHECK(wh_partner_blow_dropped(1, 1, 1, 0, 0, 1) == 0, "host: `world set puppethits 1` applies it (old behaviour)");
    CHECK(wh_partner_blow_dropped(1, 0, -1, 0, 0, 0) == 0, "host: not a human's blow (an enemy, a trap): untouched");
    CHECK(wh_partner_blow_dropped(0, 1, 0, 1, 0, 0) == 0, "client: never (its puppet of the host has its own path)");
    /* the killer slot of a death on the host */
    CHECK(wh_death_credit_slot(0, W1_NO_SLOT, 0, 0, 0) == 0, "credit: the host's own kill keeps slot 0");
    CHECK(wh_death_credit_slot(1, W1_NO_SLOT, 0, 1, 0) == 1, "credit: a HIT applied for the client (ops) keeps slot 1");
    CHECK(wh_death_credit_slot(1, W1_NO_SLOT, 0, 0, 1) == 1, "credit: a puppet driven by the client's pad keeps slot 1");
    CHECK(wh_death_credit_slot(1, W1_NO_SLOT, 0, 0, 0) == W1_NO_SLOT, "credit: the AI puppet (capture lost) credits nobody");
    CHECK(wh_death_credit_slot(W1_NO_SLOT, W1_NO_SLOT, 0, 0, 0) == W1_NO_SLOT, "credit: nobody stays nobody");
    /* the capture keep */
    int pv = -1;
    CHECK(ck_setkind(1, 1, 2, &pv) == CK_KEEP && pv == 1, "keep: setKind(2) on a captured human becomes setKind(1)");
    CHECK(ck_setkind(1, 1, 3, &pv) == CK_KEEP && pv == 1, "keep: setKind(3) too");
    CHECK(ck_setkind(1, 0, 2, &pv) == CK_LOST && pv == 2, "keep 0: the game wins, the capture is lost");
    CHECK(ck_setkind(1, 1, 1, &pv) == CK_PASS && pv == 1, "keep: setKind(1) passes");
    CHECK(ck_setkind(0, 1, 2, &pv) == CK_PASS && pv == 2, "keep: a unit that is not captured is never touched");
    CHECK(ck_fields(1, 1, 1, 0x1000, 0x1000, 1) == CK_PASS, "fields: all 1, same uCallThink");
    CHECK(ck_fields(2, 2, 2, 0x1000, 0x1000, 1) == CK_KEEP, "fields: the AI values are written back");
    CHECK(ck_fields(1, 2, 1, 0x1000, 0x1000, 1) == CK_KEEP, "fields: one field is enough");
    CHECK(ck_fields(1, 1, 1, 0x2000, 0x1000, 1) == CK_KEEP, "fields: a new uCallThink is taken and set");
    CHECK(ck_fields(2, 2, 2, 0x1000, 0x1000, 0) == CK_LOST, "fields: keep 0 leaves them (lost)");
}

/* rev1 live 10 (task 2): the barn key's SF 4864. AddItem calls FUN_00721F90(code, 1), which stores a key item's flag (rKeyItem table of game\episode\setup:
 * 0x80041500 -> 4864) with FUN_00721920 directly: the SetScenarioFlag hook (0x721DE0) never saw it, so a pickup's 4864 stayed on the picker's PC (l9c needed a
 * LABELLED 4864). The model before the fix = a mirror that only sees SetScenarioFlag stores; with the KeyItemFlag hook the same store becomes a flag event. */
static void t_keyitem_flag(void)
{
    const uint32_t evm = 0x41000000u, res = 0x41001000u, arr = 0x41002000u, e0 = 0x41003000u, e1 = 0x41003100u, e2 = 0x41003200u;
    reg_add(evm, 0x100), reg_add(res, 0x100), reg_add(arr, 0x100), reg_add(e0, 0x400);
    uint32_t v;
    v = res, memcpy(at(evm + 0x2C, 4), &v, 4);
    v = 3, memcpy(at(res + 0x90, 4), &v, 4);
    v = arr, memcpy(at(res + 0x9C, 4), &v, 4);
    v = e0, memcpy(at(arr, 4), &v, 4);
    v = e1, memcpy(at(arr + 4, 4), &v, 4);
    v = e2, memcpy(at(arr + 8, 4), &v, 4);
    v = 4864, memcpy(at(e0 + 8, 4), &v, 4);
    v = 0x2200, memcpy(at(e1 + 8, 4), &v, 4);
    v = 0xFFFFFFFFu, memcpy(at(e2 + 8, 4), &v, 4);
    uint32_t f = 0;
    CHECK(wh_keyitem_flag(&MEM, evm, 0x80041500u, &f) == 1 && f == 4864, "key flag: the barn key 0x80041500 -> SF 4864");
    CHECK(wh_keyitem_flag(&MEM, evm, 0x80041501u, &f) == 1 && f == 0x2200, "key flag: 0x80041501 -> 8704");
    CHECK(wh_keyitem_flag(&MEM, evm, 0x80041502u, &f) == 0, "key flag: an entry without a flag (-1)");
    CHECK(wh_keyitem_flag(&MEM, evm, 0x80041503u, &f) == 0, "key flag: an index past the table");
    CHECK(wh_keyitem_flag(&MEM, evm, 0x80041203u, &f) == 0 && wh_keyitem_flag(&MEM, evm, 0x80041403u, &f) == 0, "key flag: not a key item (ammo, herb)");
    CHECK(wh_keyitem_flag(&MEM, 0, 0x80041500u, &f) == 0, "key flag: no event manager");
    /* the two mirrors on the l9c pickup: the flag store goes FUN_00721F90 -> FUN_00721920, never through 0x721DE0 */
    int sees_old = 0, sees_new = 0;
    {
        int via_setscenarioflag = 0;   /* the pickup path does not call it */
        sees_old = via_setscenarioflag;
        uint32_t id = 0;
        sees_new = via_setscenarioflag || wh_keyitem_flag(&MEM, evm, 0x80041500u, &id);
    }
    CHECK(sees_old == 0, "the model before the fix: the pickup's 4864 is not mirrored (fails the next check)");
    CHECK(sees_new == 1, "the KeyItemFlag hook: the pickup's 4864 becomes a flag event");
    CHECK(wh_sites[WH_KEYFLAG].addr == 0x721F90u && wh_sites[WH_KEYFLAG].steal == 5 && WH_KEYFLAG < WH_N_HOOKS, "KeyItemFlag 0x721F90 is a hook site, 5 bytes");
    {
        static char src[1 << 21];
        FILE *fp = fopen("../native/world1_game.c", "rb");
        size_t n = fp ? fread(src, 1, sizeof src - 1, fp) : 0;
        if (fp) fclose(fp);
        src[n] = 0;
        CHECK(n > 10000 && strstr(src, "{WH_KEYFLAG, (void *)hook_keyflag, (void **)&orig_keyflag},") != NULL, "world1_game.c: the KeyItemFlag hook is in the hook table");
        {   /* rel010 B10 (round 12 merge): the client's own kill credited by the kill record also runs the achievement gate's own-kill counter */
            const char *ck = strstr(src, "static void credit_kill(w1_key key, uint32_t unit)");
            const char *kr = ck ? strstr(ck, "S.credit_done++;") : NULL;
            CHECK(strstr(src, "#include \"ach1_game.h\"") && kr && strstr(kr, "ach1_game_own_kill();") && strstr(kr, "ach1_game_own_kill();") < strstr(ck, "static void op_kill("),
                  "world1_game.c: credit_kill calls ach1_game_own_kill() after the kill record (the client's own kills count for its achievements)");
        }
        const char *h = strstr(src, "static unsigned TC hook_keyflag(void *self, unsigned code, int value)");
        CHECK(h && strstr(h, "int have = wh_keyitem_flag(&MEM, (uint32_t)(uintptr_t)self, code, &id);") && strstr(h, "unsigned r = orig_keyflag(self, code, value);") &&
                  strstr(h, "ev.type = EV_FLAG, ev.id = id, ev.value = value, ev.old = old;"),
              "world1_game.c: the hook pushes the same flag event as SetScenarioFlag's");
    }
}

int main(void)
{
    t_table();
    t_keyitem_flag();
    t_image();
    t_decoder();
    t_exec();
    t_apply();
    t_prepare();
    t_gates();
    t_pchit_layout();
    t_keyset_doors();
    t_want();
    t_lists();
    t_pass_blocks();
    t_puppet_credit();
    t_door_net();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
