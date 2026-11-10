/*
 * world1_hooks.c - the pure part of the world hooks (see world1_hooks.h). No Windows here: the offline tests link this file with guards1.c's header only.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "world1_hooks.h"

/* The bytes are those of the Steam build 13298487 (checked against its image). A "steal" count always ends on an instruction boundary (test: wh_steal_ok). */
const wh_site wh_sites[WH_N_SITES] = {
    {"SpawnChara", 0x4688F0u, 17, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0x74, 0x02, 0x00, 0x00, 0x53, 0x56, 0x8b, 0xf1, 0x57}, 6,
     "sArrange::SpawnChara(block, index, owner, flags, fromHost, filter), ret 0x18: host reports a placed enemy, client lets only the model place one"},
    {"ApplyHit", 0x6CD270u, 17, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0x14, 0x01, 0x00, 0x00, 0x53, 0x56, 0x57, 0x8b, 0xf9}, 6,
     "ApplyHit(HitInfo*), ret 4: the own human's blow on an enemy (client: sent to the host, a replica never dies from it)"},
    {"OnHit", 0x6CCF30u, 16, {0x0f, 0x57, 0xc0, 0x56, 0x8b, 0xf1, 0x0f, 0x2f, 0x86, 0x38, 0x0e, 0x00, 0x00, 0x0f, 0x82, 0xbf}, 6,
     "OnHit(HitInfo*), ret 4: death of an enemy (host reports it, client keeps a replica alive until the host says it died)"},
    {"AddItem", 0x5CA480u, 18, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0x34, 0x01, 0x00, 0x00, 0x53, 0x56, 0x57, 0x8b, 0x7d, 0x0c}, 6,
     "cBioItemPack_vf6 (0xF, uItem*, 0), ret 0xC: the own human took a world item"},
    {"KillRecord", 0x89ECA0u, 12, {0x83, 0xec, 0x14, 0x56, 0x8b, 0xf1, 0x85, 0xf6, 0x74, 0x05, 0xe8, 0x11}, 6,
     "sRankManagerResult kill record, ret 0x18: observed only (counters and log, the engine decides the credit)"},
    {"SetScenarioFlag", 0x721DE0u, 17, {0x56, 0x8b, 0xf1, 0x85, 0xf6, 0x74, 0x05, 0xe8, 0xd4, 0x3b, 0x2b, 0x00, 0x57, 0x8b, 0x7c, 0x24, 0x0c}, 5,
     "SetScenarioFlag(id, value), ret 8: counted and logged; sent to the other PC only after `world flags on`"},
    {"DeathEnemy", 0x87BC60u, 12, {0x83, 0xec, 0x34, 0x53, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x44, 0x8b, 0x87}, 6,
     "cDeathEnemy sender (victim), ret 4: host reports the death of an enemy on every death path (OnHit and the two special ones)"},
    /* round 9 (puppet authority): push edi; mov edi,ecx; cmp byte [edi+0xc],0 | jnz +6; xor al,al; pop edi; ret 4 */
    {"ZoneHitCheck", 0x6D5980u, 15, {0x57, 0x8b, 0xf9, 0x80, 0x7f, 0x0c, 0x00, 0x75, 0x06, 0x32, 0xc0, 0x5f, 0xc2, 0x04, 0x00}, 7,
     "cEventCollision::HitCheck(record), ret 4: the FSM's story zones (callers 0x74BBF0 / 0x74BFA0 only) drop the partner's puppet and count his PC's reports"},
    /* ep1-solo: the story's partner actions, run with the own slot answering 0 (the story's lead) on the PC that plays the partner (bytes: work\image
erev-image.bin) */
    {"FsmSetEnablePartner", 0x752820u, 10, {0x51, 0xa1, 0x94, 0x93, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14}, 6,
     "FsmSetEnablePartner(param{+4 enable}), ret 8: the 1-3 solo part switches Parker off / on; run in the story's view (the partner = slot 1 on both PCs)"},
    {"FsmSetPartnerWarp", 0x7534E0u, 10, {0x51, 0xa1, 0x94, 0x93, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14}, 6,
     "FsmSetPartnerWarp(param), ret 8: the AI partner's warp to the lead; story view"},
    {"FsmSetPartnerAttackStatus", 0x753590u, 10, {0x51, 0xa1, 0x94, 0x93, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14}, 6,
     "FsmSetPartnerAttackStatus(param), ret 8: the AI partner's attack flags; story view"},
    {"FsmSetPlayerPlaceStatus", 0x7536B0u, 13, {0x51, 0x56, 0x8b, 0x74, 0x24, 0x0c, 0x8b, 0x46, 0x04, 0x85, 0xc0, 0x74, 0x2e}, 6,
     "FsmSetPlayerPlaceStatus(param{+4 chara, +8 place}), ret 8: the rain place status of Jill / Parker; story view"},
    {"PcsSetPartnerGenesisSearch", 0x776C80u, 11, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0xa1, 0x94, 0x93, 0x16, 0x01}, 6,
     "PcsSetPartnerGenesisSearch(param), ret 8: the AI partner's scan target (1-2 pa-ka-scan); story view"},
    {"PcsResetPartnerGenesisSearch", 0x776D10u, 10, {0x51, 0xa1, 0x94, 0x93, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14}, 6,
     "PcsResetPartnerGenesisSearch(param), ret 8; story view"},
    /* ep1-probe E8: the prompt status getter (mov edx,[esp+4]; mov eax,edx | imul eax,eax,0x150) */
    {"PcsActionStatus", 0x75D390u, 12, {0x8b, 0x54, 0x24, 0x04, 0x8b, 0xc2, 0x69, 0xc0, 0x50, 0x01, 0x00, 0x00}, 6,
     "mPcsActionStatus[k] getter (record; k), ret 4: a prompt the partner pressed reads 2 here once (prompt mirror)"},
    /* rev1 live 10: push esi; mov esi,ecx; test esi,esi | jz +0xC; call 0x9D59C0 (lock); mov ecx,esi; call 0x9D59C0 (the same prologue as SetScenarioFlag) */
    {"KeyItemFlag", 0x721F90u, 17, {0x56, 0x8b, 0xf1, 0x85, 0xf6, 0x74, 0x0c, 0xe8, 0x24, 0x3a, 0x2b, 0x00, 0x8b, 0xce, 0xe8, 0x1d, 0x3a}, 5,
     "the key items' scenario flag (code, value), ret 8: a key pickup's flag (barn key -> SF 4864) goes into the story mirror like SetScenarioFlag"},
    /* rev1 live 11: push ebp; mov ebp,esp; and esp,-16 | sub esp,0x24; push ebx; push esi; mov esi,[ebp+8]; push edi; mov edi,ecx */
    {"ZoneLeave", 0x44F510u, 17, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x24, 0x53, 0x56, 0x8b, 0x75, 0x08, 0x57, 0x8b, 0xf9}, 6,
     "FUN_0044F510(unit, kill), ret 8: a unit put to sleep outside the own human's zone (a corpse killed, its record used up): observed"},
    /* push ebp; mov ebp,esp; and esp,-16 | sub esp,0x184; push ebx; push esi; mov esi,ecx; push edi */
    {"RearrangeOne", 0x464320u, 17, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0x84, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8b, 0xf1, 0x57}, 6,
     "FUN_00464320(+0x10, +0x14, +0x38, +0x3C), ret 0x10: ArgCharaRearrange re-creates a missing unit without SpawnChara (client: a replica when the host simulates it)"},
    /* mov eax,[0x11695C4]; test eax,eax | jnz +0x14; push 0xFEC000 ("ptr(index)"); push 0x3D; push ... */
    {"FsmRemoveArrangeChara", 0x74D3D0u, 17, {0xa1, 0xc4, 0x95, 0x16, 0x01, 0x85, 0xc0, 0x75, 0x14, 0x68, 0x00, 0xc0, 0xfe, 0x00, 0x6a, 0x3d, 0x68}, 7,
     "FsmRemoveArrangeChara exec(param), ret 8: a story removal of a placed character: observed"},
    /* sub esp,0x40; push ebx; push ebp | mov ebp,ecx; lea ecx,[ebp+0x80]; lea edx,[ebp+0x94] */
    {"ArrangeZonePass", 0x460250u, 17, {0x83, 0xec, 0x40, 0x53, 0x55, 0x8b, 0xe9, 0x8d, 0x8d, 0x80, 0x00, 0x00, 0x00, 0x8d, 0x95, 0x94, 0x00}, 5,
     "FUN_00460250(), plain ret: the per-frame zone pass of the own human (`world set watchzone 1`: the watched human's zone while this PC watches)"},
    /* push ebp; mov ebp,esp; and esp,-16 | mov eax,[0x1169394]; sub esp,0x8c; push esi */
    {"StageFloor", 0x4176B0u, 17, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0xa1, 0x94, 0x93, 0x16, 0x01, 0x81, 0xec, 0x8c, 0x00, 0x00, 0x00}, 6,
     "FUN_004176B0(), plain ret, AL: the stage part codes under the own player (stage1.h: the watching PC answers with the watched human's codes)"},
    /* call sites: verified, never detoured */
    {"Damage", 0x6CA830u, 16, {0xf3, 0x0f, 0x10, 0x81, 0x38, 0x0e, 0x00, 0x00, 0xf3, 0x0f, 0x5c, 0x44, 0x24, 0x04, 0x0f, 0x57}, 0,
     "uCharaBase::Damage(float), ret 4: the client's blow on the host's enemy and the forced death of a replica"},
    {"ItemGone", 0x5D7D00u, 12, {0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0xf9, 0x85, 0xf6, 0x74, 0x6d}, 0,
     "FUN_005D7D00(uItem), ret 4: take a world item away without a bag (the other PC's pickup)"},
    {"SetKind", 0x6CD4E0u, 12, {0x56, 0x8b, 0xf1, 0x83, 0xbe, 0x30, 0x0e, 0x00, 0x00, 0x03, 0x57, 0x8b}, 0,
     "control kind of a unit (3 = remote): makes sure a replica is one"},
    {"DamageRecv", 0x49AA50u, 16, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x81, 0xec, 0x14, 0x02, 0x00, 0x00, 0x53, 0x56, 0x57, 0x8b}, 0,
     "Raid cDamageSyncData applier (component, msg, slot), ret 8: the client applies the host's enemy blow to its own human"},
    {"UnitKill", 0x8CC9D0u, 13, {0x8b, 0x41, 0x0c, 0x8b, 0xd0, 0xc1, 0xea, 0x0d, 0xf6, 0xc2, 0x01, 0x75, 0x18}, 0,
     "cUnit::kill (vtable slot 15): removes a replica / corpse the host no longer has"},
    {"FallOutRecv", 0x455710u, 16, {0x8b, 0x0d, 0xc4, 0x95, 0x16, 0x01, 0x8b, 0x44, 0x24, 0x04, 0x56, 0x8b, 0x70, 0x08, 0x85, 0xc9}, 0,
     "Raid cFallOutData applier (msg, slot), ret 8: the same removal through the game's own path (needs sTigerTrap [0x116FFF4])"},
    {"InterpTarget", 0x6CD6B0u, 13, {0x56, 0x8b, 0xf1, 0x8b, 0x8e, 0xcc, 0x0e, 0x00, 0x00, 0x85, 0xc9, 0x74, 0x06}, 0,
     "interpolator target (pos, ang, flag), ret 0xC: smooth movement and turning of a replica"},
    {"Euler", 0x9CF880u, 15, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x4c, 0x56, 0xf3, 0x0f, 0x10, 0x41, 0x50}, 0,
     "euler angles of a unit from its quaternion (+0x50): the host's heading of an enemy"},
    {"ChangeState", 0x49C950u, 15, {0x81, 0xec, 0x78, 0x03, 0x00, 0x00, 0x56, 0x8b, 0xf1, 0x80, 0x7e, 0x2d, 0x01, 0x75, 0x0c}, 0,
     "cCharaStateManager::ChangeState (DTI, arg, args), ret 0xC: opens a door the partner opened (`world set doors 1`)"},
    {"StateType", 0x49C3E0u, 16, {0x8b, 0x49, 0x14, 0x85, 0xc9, 0x74, 0x0b, 0x8b, 0x01, 0x8b, 0x50, 0x10, 0xff, 0xd2, 0x8b, 0x40}, 0,
     "type id of the current state (mgr): door state log"},
    {"VitalRecv", 0x697CD0u, 15, {0x56, 0x8b, 0xf1, 0x8b, 0x4e, 0x50, 0x85, 0xc9, 0x74, 0x6a, 0x57, 0x8b, 0x7c, 0x24, 0x0c}, 0,
     "Raid cPlayerVitalSyncData applier (component, msg, slot), ret 8: hp of the puppet from the partner's STATE (`world set vital 1`)"},
    {"DoorOpenReq", 0x62E360u, 16, {0x83, 0x89, 0xb4, 0x0f, 0x00, 0x00, 0x04, 0x80, 0x7c, 0x24, 0x04, 0x00, 0xc7, 0x81, 0xe0, 0x0f}, 0,
     "uDoorBase open request (door; side, b), ret 8: the door the partner opened opens here by its own OpeningAuto (`world set doors 1`)"},
    {"DoorCloseReq", 0x62E3A0u, 8, {0x83, 0x89, 0xb4, 0x0f, 0x00, 0x00, 0x08, 0xc3}, 0,
     "uDoorBase close request (ECX = door), plain ret: +0xFB4 |= 8; the partner's copy of a door closes when the opener's door is closed again (wp9)"},
    {"FlagGet", 0x721E30u, 17, {0x56, 0x8b, 0xf1, 0x57, 0x85, 0xf6, 0x74, 0x05, 0xe8, 0x83, 0x3b, 0x2b, 0x00, 0x8b, 0x7c, 0x24, 0x0c}, 0,
     "scenario flag getter (id), ret 4: the value before a store and the host's digest of the Episode 1 flags (story mirror)"},
};

const char *wh_state_name(int st) { return st == WH_OFF ? "off" : st == WH_ON ? "ON" : st == WH_REFUSED ? "REFUSED" : "ERROR"; }

/* ---------------------------------------------------------------- length decoder */

static int modrm_len(const uint8_t *p, int avail)
{
    if (avail < 1) return 0;
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    if (mod == 3) return 1;
    if (rm == 4) {                     /* SIB */
        if (avail < 2) return 0;
        n++;
        if (mod == 0 && (p[1] & 7) == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n <= avail ? n : 0;
}

int wh_insn_len(const uint8_t *p, int avail, int *rel)
{
    int pre = 0, ml;
    if (rel) *rel = 0;
    while (pre < avail && (p[pre] == 0x66 || p[pre] == 0xF2 || p[pre] == 0xF3)) pre++;   /* operand size / SSE scalar prefixes */
    if (pre >= avail) return 0;
    const uint8_t *q = p + pre;
    int a = avail - pre;
    uint8_t op = q[0];
    int len = 0;
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC9 || op == 0xCC || op == 0x9C || op == 0x9D) len = 1;   /* push / pop reg, nop, leave, int3, pushf / popf */
    else if (op == 0x6A || op == 0xA8 || (op >= 0xB0 && op <= 0xB7) || (op < 0x40 && (op & 7) == 4)) len = 2;                 /* ALU al, imm8 */
    else if (op == 0x68 || op == 0xA9 || (op >= 0xB8 && op <= 0xBF) || (op < 0x40 && (op & 7) == 5)) len = 5;                 /* ALU eax, imm32 */
    else if (op == 0xA1 || op == 0xA3) len = 5;   /* mov eax, [moffs32] / mov [moffs32], eax: an absolute address, position independent (ep1-solo prologues) */
    else if (op == 0xE8 || op == 0xE9) { if (rel) *rel = 1; len = 5; }
    else if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) { if (rel) *rel = 1; len = 2; }
    else if (op == 0xC3 || op == 0xCB) { if (rel) *rel = 1; len = 1; }
    else if (op == 0xC2) { if (rel) *rel = 1; len = 3; }
    else if (op == 0x0F) {
        if (a < 2) return 0;
        uint8_t o2 = q[1];
        if (o2 >= 0x80 && o2 <= 0x8F) { if (rel) *rel = 1; len = 6; }
        else if (o2 == 0x05 || o2 == 0x0B || o2 == 0x31 || o2 == 0xA2) len = 2;
        else if ((o2 >= 0x10 && o2 <= 0x17) || (o2 >= 0x28 && o2 <= 0x2F) || (o2 >= 0x50 && o2 <= 0x7F) || (o2 >= 0x90 && o2 <= 0x9F) || (o2 >= 0x40 && o2 <= 0x4F) ||
                 o2 == 0x1F || o2 == 0xAF || o2 == 0xB6 || o2 == 0xB7 || o2 == 0xBE || o2 == 0xBF || o2 == 0xC6) {
            ml = modrm_len(q + 2, a - 2);
            if (!ml) return 0;
            len = 2 + ml + (o2 == 0xC6 ? 1 : 0);
        } else return 0;
    } else if (op == 0xFF) {
        ml = modrm_len(q + 1, a - 1);
        if (!ml) return 0;
        int reg = (q[1] >> 3) & 7;
        if (reg == 2 || reg == 3 || reg == 4 || reg == 5) { if (rel) *rel = 1; }   /* indirect call / jmp */
        len = 1 + ml;
    } else if ((op < 0x40 && (op & 7) < 4) || (op >= 0x84 && op <= 0x8B) || op == 0x8D || op == 0xD1 || op == 0xD3 || op == 0xF7 || op == 0xFE) {
        ml = modrm_len(q + 1, a - 1);
        if (!ml) return 0;
        len = 1 + ml;
        if (op == 0xF7 && ((q[1] >> 3) & 7) < 2) len += 4;   /* test r/m32, imm32 */
    } else if (op == 0x80 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0x6B || op == 0xC6) {
        ml = modrm_len(q + 1, a - 1);
        if (!ml) return 0;
        len = 1 + ml + 1;
    } else if (op == 0x81 || op == 0x69 || op == 0xC7) {
        ml = modrm_len(q + 1, a - 1);
        if (!ml) return 0;
        len = 1 + ml + 4;
    } else return 0;
    len += pre;
    return len <= avail ? len : 0;
}

int wh_steal_ok(const uint8_t *code, int avail, int steal, char *why, size_t why_size)
{
    int pos = 0;
    if (steal < 5) {
        snprintf(why, why_size, "the steal count %d is shorter than a jmp (5)", steal);
        return 0;
    }
    while (pos < steal) {
        int rel = 0, n = wh_insn_len(code + pos, avail - pos, &rel);
        if (!n) {
            snprintf(why, why_size, "unknown instruction at +%d (opcode %02x)", pos, pos < avail ? code[pos] : 0);
            return 0;
        }
        if (rel) {
            snprintf(why, why_size, "position dependent instruction at +%d (opcode %02x)", pos, code[pos]);
            return 0;
        }
        pos += n;
    }
    if (pos != steal) {
        snprintf(why, why_size, "the steal count %d cuts an instruction (the boundary is at %d)", steal, pos);
        return 0;
    }
    why[0] = 0;
    return 1;
}

/* ---------------------------------------------------------------- builders */

int wh_tramp_build(uint8_t *out, const uint8_t *stolen, int steal, uint32_t tramp_addr, uint32_t back_to)
{
    memcpy(out, stolen, (size_t)steal);
    uint32_t rel = back_to - (tramp_addr + (uint32_t)steal + 5);
    out[steal] = 0xE9;
    memcpy(out + steal + 1, &rel, 4);
    return steal + 5;
}

void wh_patch_build(uint8_t *out, int steal, uint32_t addr, uint32_t hook)
{
    uint32_t rel = hook - (addr + 5);
    memset(out, 0x90, (size_t)steal);
    out[0] = 0xE9;
    memcpy(out + 1, &rel, 4);
}

/* ---------------------------------------------------------------- apply / revert */

static void logf_(const wh_mem *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const wh_mem *m, const char *fmt, ...)
{
    char line[260];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (m && m->log) m->log(m->user, line);
}

static void hex_(char *out, size_t size, const uint8_t *b, int n)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 4 < size; i++) o += (size_t)snprintf(out + o, size - o, "%s%02x", i ? " " : "", b[i]);
    if (size && o >= size) out[size - 1] = 0;
}

static void refuse(wh_t *w, const wh_mem *m, int id, int st, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void refuse(wh_t *w, const wh_mem *m, int id, int st, const char *fmt, ...)
{
    char why[112];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    int first = w->state[id] != st || strcmp(w->why[id], why) != 0;   /* asked again every frame: a refusal is logged and counted once */
    w->state[id] = st;
    snprintf(w->why[id], sizeof w->why[id], "%s", why);
    if (first) {
        w->refusals[id]++;
        logf_(m, "world: %s hook not written: %s", wh_sites[id].name, why);
    }
}

void wh_init(wh_t *w, int env_on)
{
    memset(w, 0, sizeof *w);
    w->env_on = env_on != 0;
}

int wh_want(const wh_t *w, int net_active)
{
    if (w->mode == G1_MODE_OFF) return 0;
    if (w->mode == G1_MODE_ON) return 1;
    return w->env_on || net_active;
}

/* what the live bytes of a hook site must be when our patch is in: the patch over the first `steal` bytes, the rest of the signature as it was */
static int ours_bytes(const wh_t *w, int id, uint8_t *out)
{
    const wh_site *s = &wh_sites[id];
    if (id >= WH_N_HOOKS || !w->hook_va[id]) return 0;
    memcpy(out, s->sig, (size_t)s->sig_n);
    wh_patch_build(out, s->steal, s->addr, w->hook_va[id]);
    return 1;
}

int wh_check(wh_t *w, const wh_mem *m, int id)
{
    const wh_site *s = &wh_sites[id];
    uint8_t cur[WH_SIG_MAX], ours[WH_SIG_MAX];
    if (!m->read(m->user, s->addr, cur, (uint32_t)s->sig_n)) {
        refuse(w, m, id, WH_ERROR, "cannot read %08x", s->addr);
        return 0;
    }
    if (ours_bytes(w, id, ours) && memcmp(cur, ours, (size_t)s->sig_n) == 0) return 1;   /* our patch already */
    w->alt_seen[id] = 0;
    if (id >= WH_N_HOOKS && w->alt_n[id] > 0 && w->alt_n[id] <= s->sig_n && memcmp(cur, w->alt[id], (size_t)w->alt_n[id]) == 0 &&
        memcmp(cur + w->alt_n[id], s->sig + w->alt_n[id], (size_t)(s->sig_n - w->alt_n[id])) == 0) {
        w->state[id] = WH_ON;   /* the known detour of another module of this DLL, the rest of the function as expected: called through it */
        w->why[id][0] = 0;
        w->alt_seen[id] = 1;
        return 1;
    }
    if (memcmp(cur, s->sig, (size_t)s->sig_n) != 0) {
        char hx[80];
        hex_(hx, sizeof hx, cur, s->sig_n > 12 ? 12 : s->sig_n);
        refuse(w, m, id, WH_REFUSED, "bytes at %08x are %s ...", s->addr, hx);
        return 0;
    }
    if (id < WH_N_HOOKS) {
        char why[100];
        if (!wh_steal_ok(s->sig, s->sig_n, s->steal, why, sizeof why)) {
            refuse(w, m, id, WH_REFUSED, "%s", why);
            return 0;
        }
    } else {
        w->state[id] = WH_ON;   /* a call site whose bytes are right */
        w->why[id][0] = 0;
    }
    return 1;
}

void wh_set_alt(wh_t *w, int id, const uint8_t *bytes, int n)
{
    if (id < WH_N_HOOKS || id >= WH_N_SITES) return;   /* only call sites: a hook site with someone else's detour is never patched over */
    if (!bytes || n <= 0 || n > WH_ALT_MAX) n = 0;
    w->alt_n[id] = n;
    memset(w->alt[id], 0, sizeof w->alt[id]);
    if (n) memcpy(w->alt[id], bytes, (size_t)n);
}

int wh_prepare(wh_t *w, const wh_mem *m, int id)
{
    if (id < 0 || id >= WH_N_HOOKS) return 0;
    const wh_site *s = &wh_sites[id];
    if (!wh_check(w, m, id)) return 0;
    if (!w->tramp[id]) {
        uint32_t blk = m->alloc ? m->alloc(m->user, 32) : 0;
        if (!blk) {
            refuse(w, m, id, WH_ERROR, "no executable memory for the trampoline");
            return 0;
        }
        uint8_t t[32];
        int n = wh_tramp_build(t, s->sig, s->steal, blk, s->addr + (uint32_t)s->steal);
        if (!m->write(m->user, blk, t, (uint32_t)n)) {
            refuse(w, m, id, WH_ERROR, "cannot fill the trampoline");
            return 0;
        }
        w->tramp[id] = blk;
    }
    return 1;
}

int wh_apply(wh_t *w, const wh_mem *m, int id, uint32_t hook_va)
{
    if (id < 0 || id >= WH_N_HOOKS) return 0;
    const wh_site *s = &wh_sites[id];
    uint8_t cur[WH_SIG_MAX], ours[WH_SIG_MAX];
    if (!m->read(m->user, s->addr, cur, (uint32_t)s->sig_n)) {
        refuse(w, m, id, WH_ERROR, "cannot read %08x", s->addr);
        return 0;
    }
    if (w->hook_va[id] == hook_va && ours_bytes(w, id, ours) && memcmp(cur, ours, (size_t)s->sig_n) == 0) {
        w->state[id] = WH_ON;
        return 1;
    }
    if (!wh_prepare(w, m, id)) return 0;
    w->hook_va[id] = hook_va;
    ours_bytes(w, id, ours);
    if (!m->write(m->user, s->addr, ours, (uint32_t)s->steal)) {   /* the trampoline is ready; one write of the steal bytes switches the site */
        refuse(w, m, id, WH_ERROR, "write to %08x failed", s->addr);
        return 0;
    }
    w->state[id] = WH_ON, w->why[id][0] = 0, w->applies[id]++;
    logf_(m, "world: %s hook written: %08x jmp %08x (trampoline %08x, %d bytes stolen)", s->name, s->addr, hook_va, (unsigned)w->tramp[id], s->steal);
    return 1;
}

int wh_revert(wh_t *w, const wh_mem *m, int id)
{
    if (id < 0 || id >= WH_N_HOOKS) return 0;
    const wh_site *s = &wh_sites[id];
    uint8_t cur[WH_SIG_MAX], ours[WH_SIG_MAX];
    if (!m->read(m->user, s->addr, cur, (uint32_t)s->sig_n)) return 0;
    if (memcmp(cur, s->sig, (size_t)s->sig_n) == 0) {   /* original already */
        w->state[id] = WH_OFF;
        return 1;
    }
    if (!ours_bytes(w, id, ours) || memcmp(cur, ours, (size_t)s->sig_n) != 0) {   /* somebody else changed it: leave it alone */
        refuse(w, m, id, WH_REFUSED, "revert: the bytes are not ours, left alone");
        return 0;
    }
    if (!m->write(m->user, s->addr, s->sig, (uint32_t)s->steal)) return 0;
    w->state[id] = WH_OFF, w->why[id][0] = 0, w->reverts[id]++;
    logf_(m, "world: %s hook removed (%08x original again)", s->name, s->addr);
    return 1;
}

int wh_probe(const wh_t *w, const wh_mem *m, int id, char *out, size_t size)
{
    const wh_site *s = &wh_sites[id];
    uint8_t cur[WH_SIG_MAX], ours[WH_SIG_MAX];
    if (!m->read(m->user, s->addr, cur, (uint32_t)s->sig_n)) {
        snprintf(out, size, "unreadable");
        return WH_ERROR;
    }
    if (memcmp(cur, s->sig, (size_t)s->sig_n) == 0) {
        snprintf(out, size, "original");
        return WH_OFF;
    }
    if (ours_bytes(w, id, ours) && memcmp(cur, ours, (size_t)s->sig_n) == 0) {
        snprintf(out, size, "patched (jmp %08x)", (unsigned)w->hook_va[id]);
        return WH_ON;
    }
    char hx[80];
    hex_(hx, sizeof hx, cur, s->sig_n > 12 ? 12 : s->sig_n);
    snprintf(out, size, "FOREIGN %s", hx);
    return WH_REFUSED;
}

void wh_status(const wh_t *w, const wh_mem *m, int net_active, char *out, size_t size)
{
    static const char *const mode[] = {"auto", "forced on", "forced off"};
    size_t o = 0;
    o += (size_t)snprintf(out + o, size - o, "world hooks: mode %s, REV1_WORLD %d, network session %s, wanted %s", mode[w->mode >= 0 && w->mode < 3 ? w->mode : 0], w->env_on, net_active ? "ACTIVE" : "no",
                          wh_want(w, net_active) ? "on" : "off");
    for (int i = 0; i < WH_N_SITES && o < size; i++) {
        char live[96];
        wh_probe(w, m, i, live, sizeof live);
        o += (size_t)snprintf(out + o, size - o, "\n  %-15s %08x %s: state %s, live bytes %s; applied %u, reverted %u, refused %u%s%s%s", wh_sites[i].name, wh_sites[i].addr, i < WH_N_HOOKS ? "hook" : "call",
                              wh_state_name(w->state[i]), live, w->applies[i], w->reverts[i], w->refusals[i], w->why[i][0] ? ", why: " : "", w->why[i],
                              w->alt_seen[i] ? " (accepted: the known detour of another module of this DLL at the entry, called through it)" : "");
    }
}

/* ---------------------------------------------------------------- engine structures */

int wh_rd32(const wh_mem *m, uint32_t addr, uint32_t *out)
{
    uint32_t v = 0;
    if (!addr || !m->read(m->user, addr, &v, 4)) return 0;
    *out = v;
    return 1;
}

int wh_rdf(const wh_mem *m, uint32_t addr, float *out)
{
    float v = 0;
    if (!addr || !m->read(m->user, addr, &v, 4)) return 0;
    *out = v;
    return 1;
}

int wh_block_ids(const wh_mem *m, uint32_t sarr, unsigned block, uint32_t out[3])
{
    uint32_t bl, cnt, arr, blk;
    if (!wh_rd32(m, sarr + WH_SARR_BLOCKS, &bl) || !wh_rd32(m, bl + 0x74, &cnt) || block >= cnt || !wh_rd32(m, bl + 0x80, &arr) || !wh_rd32(m, arr + 4u * block, &blk) || !blk) return 0;
    return wh_rd32(m, blk + 0x10, &out[0]) && wh_rd32(m, blk + 0x14, &out[1]) && wh_rd32(m, blk + 0x1C, &out[2]);
}

int wh_char_class(const wh_mem *m, uint32_t sarr, unsigned block, unsigned index, uint32_t *cls)
{
    uint32_t bl, cnt, arr, blk, chars, ccnt, carr, rec;
    if (!wh_rd32(m, sarr + WH_SARR_BLOCKS, &bl) || !wh_rd32(m, bl + 0x74, &cnt) || block >= cnt || !wh_rd32(m, bl + 0x80, &arr) || !wh_rd32(m, arr + 4u * block, &blk) || !blk) return 0;
    if (!wh_rd32(m, blk + 4, &chars) || !chars || !wh_rd32(m, chars + 0x74, &ccnt) || index >= ccnt || !wh_rd32(m, chars + 0x80, &carr) || !wh_rd32(m, carr + 4u * index, &rec) || !rec) return 0;
    return wh_rd32(m, rec + 8, cls);
}

unsigned wh_list_count(const wh_mem *m, uint32_t sarr, uint32_t list_off)
{
    uint32_t n;
    if (!wh_rd32(m, sarr + list_off + 4, &n) || n > 4096) return 0;
    return n;
}

int wh_list_at(const wh_mem *m, uint32_t sarr, uint32_t list_off, unsigned i, wh_entry *out)
{
    uint32_t arr, e;
    if (i >= wh_list_count(m, sarr, list_off) || !wh_rd32(m, sarr + list_off + 0x10, &arr) || !wh_rd32(m, arr + 4u * i, &e) || !e) return 0;
    out->entry = e;
    return wh_rd32(m, e + 4, &out->unit) && wh_rd32(m, e + 0x10, &out->k[0]) && wh_rd32(m, e + 0x14, &out->k[1]) && wh_rd32(m, e + 0x38, &out->k[2]) && wh_rd32(m, e + 0x3C, &out->k[3]);
}

int wh_find_key(const wh_mem *m, uint32_t sarr, uint32_t list_off, unsigned block, unsigned index, wh_entry *out)
{
    uint32_t ids[3];
    if (!wh_block_ids(m, sarr, block, ids)) return 0;
    uint32_t k4[4] = {ids[0], ids[1], ids[2], index};
    return wh_find_key4(m, sarr, list_off, k4, out);
}

int wh_find_unit(const wh_mem *m, uint32_t sarr, uint32_t list_off, uint32_t unit, wh_entry *out)
{
    unsigned n = wh_list_count(m, sarr, list_off);
    wh_entry e;
    for (unsigned i = 0; i < n; i++)
        if (wh_list_at(m, sarr, list_off, i, &e) && e.unit == unit) {
            *out = e;
            return 1;
        }
    return 0;
}

int wh_find_key4(const wh_mem *m, uint32_t sarr, uint32_t list_off, const uint32_t k4[4], wh_entry *out)
{
    unsigned n = wh_list_count(m, sarr, list_off);
    wh_entry e;
    for (unsigned i = 0; i < n; i++)
        if (wh_list_at(m, sarr, list_off, i, &e) && memcmp(e.k, k4, sizeof e.k) == 0) {
            *out = e;
            return 1;
        }
    return 0;
}

int wh_entry_key(const wh_mem *m, uint32_t sarr, const wh_entry *e, unsigned *block, unsigned *index)
{
    uint32_t bl, cnt, ids[3];
    if (!wh_rd32(m, sarr + WH_SARR_BLOCKS, &bl) || !wh_rd32(m, bl + 0x74, &cnt) || cnt > 1024) return 0;
    for (unsigned b = 0; b < cnt; b++)
        if (wh_block_ids(m, sarr, b, ids) && ids[0] == e->k[0] && ids[1] == e->k[1] && ids[2] == e->k[2]) {
            *block = b, *index = e->k[3];
            return 1;
        }
    return 0;
}

unsigned wh_block_table(const wh_mem *m, uint32_t sarr, wh_block *out, unsigned max)
{
    uint32_t bl, cnt;
    if (!wh_rd32(m, sarr + WH_SARR_BLOCKS, &bl) || !wh_rd32(m, bl + 0x74, &cnt) || cnt > max) return 0;
    unsigned n = 0;
    for (unsigned b = 0; b < cnt; b++) {
        if (!wh_block_ids(m, sarr, b, out[b].ids)) memset(out[b].ids, 0xFF, sizeof out[b].ids);   /* a hole: matches nothing real */
        n++;
    }
    return n;
}

int wh_key_in_table(const wh_block *t, unsigned n, const wh_entry *e, unsigned *block, unsigned *index)
{
    for (unsigned b = 0; b < n; b++)
        if (t[b].ids[0] == e->k[0] && t[b].ids[1] == e->k[1] && t[b].ids[2] == e->k[2]) {
            *block = b, *index = e->k[3];
            return 1;
        }
    return 0;
}

int wh_key_listed(const wh_mem *m, uint32_t sarr, unsigned block, unsigned index, wh_entry *out)
{
    wh_entry e;
    uint32_t rc = 0;
    uint8_t spent = 1;
    if (!wh_find_key(m, sarr, WH_LIST_ENEMIES, block, index, &e)) return 0;
    if (out) *out = e;
    if (e.unit) return wh_class_is_enemy(wh_unit_class(m, e.unit));
    if (!m->read(m->user, e.entry + WH_ENTRY_SPENT, &spent, 1) || spent) return 0;
    return wh_char_class(m, sarr, block, index, &rc) && wh_class_is_enemy(rc);
}

uint32_t wh_unit_class(const wh_mem *m, uint32_t unit)
{
    uint32_t c;
    if (!unit || !wh_rd32(m, unit + WH_U_CLASS, &c) || c == 0x80000000u) return 0;
    return c;
}

int wh_class_is_enemy(uint32_t cls) { return (cls & 0xF00F0000u) == 0x80030000u; }
int wh_class_is_player(uint32_t cls) { return (cls & 0xF00F0000u) == 0x80010000u; }

int wh_player_by_slot(const wh_mem *m, unsigned slot, uint32_t *unit)
{
    uint32_t sp, head, endp, u, ser;
    if (!wh_rd32(m, WH_SPLAYER_PTR, &sp) || !sp || !wh_rd32(m, sp + 0x30, &head) || !wh_rd32(m, sp + 0x34, &endp)) return 0;
    for (int guard = 0; head && head != endp && guard < 40; guard++) {
        if (wh_rd32(m, head + 0x10, &u) && u && wh_class_is_player(wh_unit_class(m, u)) && wh_rd32(m, u + WH_U_SERIAL, &ser) && ser == slot) {
            *unit = u;
            return 1;
        }
        if (!wh_rd32(m, head + 8, &head)) break;
    }
    return 0;
}

int wh_keyitem_flag(const wh_mem *m, uint32_t evmgr, uint32_t code, uint32_t *flag)
{
    uint32_t res, n, arr, e, f;
    if ((code & WH_KEYITEM_MASK) != WH_KEYITEM_CODE || !evmgr) return 0;
    if (!wh_rd32(m, evmgr + 0x2C, &res) || !res || !wh_rd32(m, res + 0x90, &n) || (code & 0xFFu) >= n) return 0;
    if (!wh_rd32(m, res + 0x9C, &arr) || !arr || !wh_rd32(m, arr + (code & 0xFFu) * 4u, &e) || !e || !wh_rd32(m, e + 8, &f) || f == 0xFFFFFFFFu) return 0;
    *flag = f;
    return 1;
}

int wh_hit_human(const wh_mem *m, uint32_t hit, uint32_t *unit, int *slot, int *kind)
{
    static const uint32_t off[2] = {0x40, 0x44};
    for (int i = 0; i < 2; i++) {
        uint32_t u, ser, kd;
        if (wh_rd32(m, hit + off[i], &u) && u && wh_class_is_player(wh_unit_class(m, u)) && wh_rd32(m, u + WH_U_SERIAL, &ser) && wh_rd32(m, u + WH_U_KIND, &kd)) {
            if (unit) *unit = u;
            if (slot) *slot = (int)ser;
            if (kind) *kind = (int)kd;
            return 1;
        }
    }
    return 0;
}

int wh_partner_blow_dropped(int host, int human, int hslot, int own, int in_ops, int puppethits)
{
    return host && human && hslot != own && !in_ops && !puppethits;
}

int wh_death_credit_slot(int slot, int no_slot, int own, int in_ops, int pad_driven)
{
    if (slot == no_slot || slot == own || in_ops || pad_driven) return slot;
    return no_slot;
}

void wh_hit_build(uint8_t buf[WH_HIT_SIZE], uint32_t vtbl, uint32_t attacker, uint32_t owner, uint32_t flags, const float pt[3])
{
    memset(buf, 0, WH_HIT_SIZE);
    memcpy(buf, &vtbl, 4);
    if (pt) memcpy(buf + 0x10, pt, 12);
    memcpy(buf + 0x40, &attacker, 4);
    memcpy(buf + 0x44, &owner, 4);
    memcpy(buf + 0x54, &flags, 4);
}

int wh_spawn_result(const wh_mem *m, uint32_t sarr, unsigned block, unsigned index, uint32_t *unit)
{
    wh_entry e;
    if (unit) *unit = 0;
    if (!sarr || !wh_find_key(m, sarr, WH_LIST_ENEMIES, block, index, &e)) return WH_SP_NONE;
    if (!e.unit || !wh_class_is_enemy(wh_unit_class(m, e.unit))) return WH_SP_NOUNIT;   /* NULL (another area) or not readable as an enemy: never handed to the engine */
    if (unit) *unit = e.unit;
    return WH_SP_UNIT;
}

int wh_replica_owner(int engine_own, int host_slot)
{
    if (host_slot != engine_own && host_slot >= 0 && host_slot <= 1) return host_slot;
    return engine_own == 0 ? 1 : 0;
}

int wh_snap_needed(const float cur[3], const float tgt[3], float thr)
{
    if (!(thr > 0.0f)) return 0;
    for (int i = 0; i < 3; i++)
        if (cur[i] != cur[i] || tgt[i] != tgt[i]) return 0;
    float dx = tgt[0] - cur[0], dy = tgt[1] - cur[1], dz = tgt[2] - cur[2];
    return dx * dx + dz * dz > thr * thr || dy > WH_SNAP_DY || dy < -WH_SNAP_DY;
}

int wh_unit_vfn(const wh_mem *m, uint32_t unit, uint32_t off, uint32_t *fn)
{
    uint32_t vt;
    if (!unit || !wh_rd32(m, unit, &vt) || !vt || !wh_rd32(m, vt + off, fn) || !*fn) return 0;
    return 1;
}

/* ---------------------------------------------------------------- interpolator gates */
const wh_gate_site wh_gates[WH_N_GATES] = {
    {"InterpTick", 0x4A0DA8u, 19, {0x8b, 0x0d, 0x10, 0x05, 0x1e, 0x01, 0xe8, 0x3d, 0x0f, 0x3e, 0x00, 0x84, 0xc0, 0x0f, 0x84, 0x4e, 0x09, 0x00, 0x00}, 13, 6, {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}},
    {"InterpTarget", 0x4A09C8u, 19, {0x8b, 0x0d, 0x10, 0x05, 0x1e, 0x01, 0xe8, 0x1d, 0x13, 0x3e, 0x00, 0x84, 0xc0, 0x0f, 0x84, 0x50, 0x01, 0x00, 0x00}, 13, 6, {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}},
    {"InterpQueue", 0x49FA58u, 19, {0x8b, 0x0d, 0x10, 0x05, 0x1e, 0x01, 0xe8, 0x8d, 0x22, 0x3e, 0x00, 0x84, 0xc0, 0x0f, 0x84, 0x52, 0x03, 0x00, 0x00}, 13, 6, {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}},
};

static void gate_ours(int id, uint8_t *out)
{
    const wh_gate_site *g = &wh_gates[id];
    memcpy(out, g->orig, (size_t)g->n);
    memcpy(out + g->off, g->patch, (size_t)g->pn);
}

static void gate_refuse(wh_gates_t *g, const wh_mem *m, int id, int st, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void gate_refuse(wh_gates_t *g, const wh_mem *m, int id, int st, const char *fmt, ...)
{
    char why[112];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    int first = g->state[id] != st || strcmp(g->why[id], why) != 0;
    g->state[id] = st;
    snprintf(g->why[id], sizeof g->why[id], "%s", why);
    if (first) {
        g->refusals[id]++;
        logf_(m, "world: %s gate not patched: %s", wh_gates[id].name, why);
    }
}

int wh_gate_apply(wh_gates_t *g, const wh_mem *m, int id)
{
    if (id < 0 || id >= WH_N_GATES) return 0;
    const wh_gate_site *s = &wh_gates[id];
    uint8_t cur[24], ours[24];
    gate_ours(id, ours);
    if (!m->read(m->user, s->addr, cur, (uint32_t)s->n)) {
        gate_refuse(g, m, id, WH_ERROR, "cannot read %08x", s->addr);
        return 0;
    }
    if (memcmp(cur, ours, (size_t)s->n) == 0) {
        g->state[id] = WH_ON;
        return 1;
    }
    if (memcmp(cur, s->orig, (size_t)s->n) != 0) {
        char hx[80];
        hex_(hx, sizeof hx, cur, s->n);
        gate_refuse(g, m, id, WH_REFUSED, "bytes at %08x are %s", s->addr, hx);
        return 0;
    }
    if (!m->write(m->user, s->addr + (uint32_t)s->off, s->patch, (uint32_t)s->pn)) {
        gate_refuse(g, m, id, WH_ERROR, "write to %08x failed", s->addr + (uint32_t)s->off);
        return 0;
    }
    g->state[id] = WH_ON, g->why[id][0] = 0, g->applies[id]++;
    logf_(m, "world: %s gate patched: %08x jz -> 6-byte nop (the interpolator runs offline for view control 3)", s->name, s->addr + (uint32_t)s->off);
    return 1;
}

int wh_gate_revert(wh_gates_t *g, const wh_mem *m, int id)
{
    if (id < 0 || id >= WH_N_GATES) return 0;
    const wh_gate_site *s = &wh_gates[id];
    uint8_t cur[24], ours[24];
    gate_ours(id, ours);
    if (!m->read(m->user, s->addr, cur, (uint32_t)s->n)) return 0;
    if (memcmp(cur, s->orig, (size_t)s->n) == 0) {
        g->state[id] = WH_OFF;
        return 1;
    }
    if (memcmp(cur, ours, (size_t)s->n) != 0) {
        gate_refuse(g, m, id, WH_REFUSED, "revert: the bytes are not ours, left alone");
        return 0;
    }
    if (!m->write(m->user, s->addr + (uint32_t)s->off, s->orig + s->off, (uint32_t)s->pn)) return 0;
    g->state[id] = WH_OFF, g->why[id][0] = 0, g->reverts[id]++;
    logf_(m, "world: %s gate restored (%08x original jz)", s->name, s->addr + (uint32_t)s->off);
    return 1;
}

int wh_gate_probe(const wh_mem *m, int id, char *out, size_t size)
{
    const wh_gate_site *s = &wh_gates[id];
    uint8_t cur[24], ours[24];
    gate_ours(id, ours);
    if (!m->read(m->user, s->addr, cur, (uint32_t)s->n)) {
        snprintf(out, size, "unreadable");
        return WH_ERROR;
    }
    if (memcmp(cur, s->orig, (size_t)s->n) == 0) {
        snprintf(out, size, "original");
        return WH_OFF;
    }
    if (memcmp(cur, ours, (size_t)s->n) == 0) {
        snprintf(out, size, "patched");
        return WH_ON;
    }
    char hx[80];
    hex_(hx, sizeof hx, cur, s->n);
    snprintf(out, size, "FOREIGN %s", hx);
    return WH_REFUSED;
}

/* ---------------------------------------------------------------- PCHIT (cDamageSyncData layout) */
static void cp32(uint8_t *d, const uint8_t *s) { memcpy(d, s, 4); }

void wh_dmgmsg_from_hit(const uint8_t hit[WH_HIT_SIZE], uint16_t att_serial, uint16_t own_serial, uint8_t msg[WH_DMGMSG_SIZE])
{
    memset(msg, 0, WH_DMGMSG_SIZE);
    for (int v = 0; v < 3; v++)
        for (int k = 0; k < 3; k++) cp32(msg + 0x10 + 0x10 * v + 4 * k, hit + 0x10 + 0x10 * v + 4 * k);
    memcpy(msg + 0x40, &att_serial, 2);
    memcpy(msg + 0x42, &own_serial, 2);
    msg[0x44] = hit[0x48], msg[0x45] = hit[0x4C];
    cp32(msg + 0x48, hit + 0x50), cp32(msg + 0x4C, hit + 0x54);
    msg[0x50] = hit[0x58], msg[0x51] = hit[0x5C];
    for (int k = 0; k < 6; k++) cp32(msg + 0x54 + 4 * k, hit + 0x64 + 4 * k);   /* +0x54..+0x68 <- +0x64..+0x78 */
    msg[0x6C] = hit[0x7C];
    cp32(msg + 0x70, hit + 0x80), cp32(msg + 0x74, hit + 0x84);
    msg[0x78] = 0;   /* no extra hits: the array +0x80/+0x84 stays empty */
}

void wh_pchit_pack(const uint8_t msg[WH_DMGMSG_SIZE], uint8_t out[WH_PCHIT_LEN])
{
    memcpy(out, msg + 0x10, 12);
    memcpy(out + 12, msg + 0x20, 12);
    memcpy(out + 24, msg + 0x30, 12);
    memcpy(out + 36, msg + 0x44, 0x78 - 0x44);   /* 52 bytes */
}

void wh_pchit_unpack(const uint8_t in[WH_PCHIT_LEN], uint16_t att_serial, uint16_t own_serial, uint8_t msg[WH_DMGMSG_SIZE])
{
    memset(msg, 0, WH_DMGMSG_SIZE);
    msg[4] = 2;   /* cRemoteCall mode word as the Raid senders set it; the applier reads neither +0 nor +4 */
    memcpy(msg + 0x10, in, 12);
    memcpy(msg + 0x20, in + 12, 12);
    memcpy(msg + 0x30, in + 24, 12);
    memcpy(msg + 0x40, &att_serial, 2);
    memcpy(msg + 0x42, &own_serial, 2);
    memcpy(msg + 0x44, in + 36, 0x78 - 0x44);
    msg[0x78] = 0;
}

/* ---------------------------------------------------------------- key set */
void wh_keyset_clear(wh_keyset *s) { memset(s->bits, 0, sizeof s->bits); }
int wh_keyset_first(wh_keyset *s, uint16_t key)
{
    uint8_t bit = (uint8_t)(1u << (key & 7));
    if (s->bits[key >> 3] & bit) return 0;
    s->bits[key >> 3] |= bit;
    return 1;
}

/* ---------------------------------------------------------------- door states */
const wh_door_state wh_door_states[WH_N_DS] = {
    {"ClosedState", 0x1175740u, 0x10321F0u},         {"KnobHandOnPlayerState", 0x11757E0u, 0x10321FCu}, {"OpeningPlayerState", 0x11758A0u, 0x1032228u},
    {"OpeningAutoState", 0x1175860u, 0x1032254u},    {"OpenedState", 0x1175720u, 0x1032268u},          {"DoorGimmickMoveState", 0x11757C0u, 0x10322E8u},
};

int wh_door_player_open(const uint32_t ds[WH_N_DS], uint32_t old_id, uint32_t new_id)
{
    if (!new_id || old_id == new_id || !ds[WH_DS_OPENING_PLAYER]) return 0;
    if (new_id == ds[WH_DS_OPENING_PLAYER]) return 1;   /* the knob turned, the door opens for the own human */
    return ds[WH_DS_KNOB] && old_id == ds[WH_DS_KNOB] && new_id == ds[WH_DS_OPENED];   /* OpeningPlayer was shorter than one scan */
}

int wh_door_own_closed(const uint32_t ds[WH_N_DS], uint32_t old_id, uint32_t new_id, int own_open)
{
    return own_open && ds[WH_DS_CLOSED] && new_id == ds[WH_DS_CLOSED] && old_id != new_id;
}

int wh_door_close_wanted(const uint32_t ds[WH_N_DS], uint32_t here_id)
{
    return here_id && ((ds[WH_DS_OPENED] && here_id == ds[WH_DS_OPENED]) || (ds[WH_DS_OPENING_AUTO] && here_id == ds[WH_DS_OPENING_AUTO]));
}

uint8_t wh_door_phase(uint32_t fb4) { return (uint8_t)(1u + (fb4 & 1u)); }

uint8_t wh_door_phase_next(uint8_t stored_phase, uint32_t fb4) { return (uint8_t)(((((unsigned)stored_phase >> 2) + 1u) & 0x3Fu) << 2 | wh_door_phase(fb4)); }

void wh_door_req_args(uint8_t phase, int *a, int *b)
{
    *a = (phase & 3u) == 1 ? 0 : 1;   /* bit 0 clear on the opener's door -> mode 3 (bit 0 clear, motion 5); set (or unknown) -> mode 1 */
    *b = 1;                    /* mode 1 / 3 (2 / 4 behave the same in OpeningAuto's enter) */
}

int wh_door_dti(const wh_mem *m, int which, uint32_t *id)
{
    const wh_door_state *d;
    uint32_t vt, nm, par;
    if (which < 0 || which >= WH_N_DS) return 0;
    d = &wh_door_states[which];
    if (!wh_rd32(m, d->dti, &vt) || vt < WH_DTI_VT_LO || vt >= WH_DTI_VT_HI || !wh_rd32(m, d->dti + 4, &nm) || nm != d->name_str || !wh_rd32(m, d->dti + 0x10, &par) || par != WH_DOORSTATE_PARENT ||
        !wh_rd32(m, d->dti + 0x1C, id) || !*id)
        return 0;
    return 1;
}
