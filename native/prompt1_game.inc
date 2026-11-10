/*
 * prompt1_game.inc - included by world1_game.c after story1_game.inc (uses its statics): E8, a story prompt pressed on one PC counts on both (prompt1.h,
 * notes\ep1-probe.md). Engine facts (static, build 13298487):
 *   sPcsManager [0x116A34C]: slot i (0..15) runs the uPcsFsm [mgr + 0x284 + 4i]; its uFsm part is at fsm + 0x30 (FUN_008916E0: `lea ecx,[esi+0x30]; call
 *   0xA25480` / `lea ecx,[eax+0x30]; call 0xA221A0`): the record (cFsmActionPcs, 0x1370 bytes) [mgr + 0x23C] + i * 0x1370 = [fsm + 0x30 + 4], the resource
 *   [[fsm + 0x30 + 0xC] + 8], its path at resource + 8 (the name the barrier's pcs hook logs). (e1p2 live: with + 4 / + 0xC no slot was ever read.)
 *   The record's six action commands (cActionCommand, vtable 0xFEBBA8, ctor 0x40FEA0) are at record + 0x9E0 + k * 0x150; mPcsActionStatus[k] = command + 0xC
 *   (getter 0x75D390); a prompt FSM waits for 2. The command keeps its status until the next pad edge of the local pad (cActionCommand_vf5 0x40FFB0).
 *   sActionCommand [0x1169390] lists the registered (shown) commands: nodes from [m + 0x38] to [m + 0x3C], next at +8, the command at +0x10.
 * Each logic pass while the model is bound and the level plays: the 16 slots are read, a rise of a status to 2 is announced (CP_W1_PROMPT), a remote press is
 * written as status 2 into the shown command of the same script here. ON in a session by default; `world pr off` (lab) stops it.
 */
#include "prompt1.h"

#define PR_PCSMAN 0x116A34Cu
#define PR_ACMAN 0x1169390u
#define PR_REC_SIZE 0x1370u
#define PR_CMD_BASE 0x9E0u
#define PR_CMD_SIZE 0x150u
#define PR_CMD_STATUS 0xCu
#define PR_CMD_VTBL 0xFEBBA8u
#define PR_UFSM 0x30u              /* uPcsFsm -> its uFsm part */

static pr1_t PR;
/* the getter hook (WH_PCSACT 0x75D390, game thread = the FSM's update): a forced press is answered 2 the next time the FSM asks for that command's status.
 * e1p3 live: writing the command's status alone was undone by the action command manager (its vf5 runs before the FSM in the frame) - the client's copy saw
 * the press logged ("pressed here too") but stayed at its prompt. */
typedef unsigned (TC *pcsact_fn)(void *self, unsigned k);
static pcsact_fn orig_pcsact;
static volatile uint32_t pr_force_rec, pr_force_k, pr_force_until;
static volatile LONG pr_force_answered;
static unsigned TC hook_pcsact(void *self, unsigned k)
{
    unsigned r = orig_pcsact(self, k);
    uint32_t rec = (uint32_t)(uintptr_t)self;
    if (pr_force_rec && rec == pr_force_rec && k == pr_force_k && k < PR1_CMDS) {
        if ((int32_t)(GetTickCount() - pr_force_until) < 0) {
            uint32_t two = PR1_PRESSED;
            wr(rec + 0x11C0 + 4 * k, &two, 4);   /* the getter's own copy (record + 0x11C0 + 4k) */
            r = PR1_PRESSED;
            InterlockedIncrement(&pr_force_answered);
        }
        pr_force_rec = 0;
    }
    return r;
}
static volatile int f_prompt = 1;
static unsigned pr_logged;
static uint32_t pr_last_epoch = 0xFFFFFFFFu;

static int pr_name(uint32_t res, char *out, unsigned cap)   /* one read of the path (64, else 32 bytes: the end of a page) */
{
    char b[64];
    unsigned n = sizeof b;
    if (!r1_safe_read((const void *)(uintptr_t)(res + 8), b, n)) {
        n = 32;
        if (!r1_safe_read((const void *)(uintptr_t)(res + 8), b, n)) return 0;
    }
    unsigned i = 0;
    for (; i + 1 < cap && i < n; i++) {
        char c = b[i];
        if (!c) break;
        if ((unsigned char)c < 32 || (unsigned char)c > 126) return 0;
        out[i] = c;
    }
    out[i] = 0;
    return i > 3;
}
static struct { uint32_t res, hash; char name[64]; } pr_cache[PR1_SLOTS];   /* the slot's resource as read last (the path is read once per resource) */

/* the registered commands of the action command manager (a bounded, fault-safe walk of its list) */
static unsigned pr_registered(uint32_t *cmds, unsigned cap)
{
    uint32_t m = rd32(PR_ACMAN), node, end, n = 0;
    if (!m) return 0;
    node = rd32(m + 0x38), end = rd32(m + 0x3C);
    for (unsigned guard = 0; node && node != end && guard < 128 && n < cap; guard++) {
        uint32_t c = rd32(node + 0x10);
        if (c) cmds[n++] = c;
        node = rd32(node + 8);
    }
    return n;
}

/* the 16 slots now; names[] for the log */
static void pr_read(pr1_view *v, char names[PR1_SLOTS][64], uint32_t recs[PR1_SLOTS])
{
    memset(v, 0, sizeof(pr1_view) * PR1_SLOTS);
    uint32_t mgr = rd32(PR_PCSMAN), base = mgr ? rd32(mgr + 0x23C) : 0, reg[128];
    unsigned nreg = pr_registered(reg, 128);
    for (unsigned s = 0; s < PR1_SLOTS; s++) {
        names[s][0] = 0, recs[s] = 0;
        uint32_t fsm = mgr ? rd32(mgr + 0x284 + 4 * s) : 0;
        if (!fsm || !base) continue;
        uint32_t rec = base + s * PR_REC_SIZE, arr = rd32(fsm + PR_UFSM + 0xC), res = arr ? rd32(arr + 8) : 0;
        if (rd32(fsm + PR_UFSM + 4) != rec || !res) continue;
        if (pr_cache[s].res != res) {
            pr_cache[s].res = res, pr_cache[s].hash = 0;
            if (pr_name(res, pr_cache[s].name, sizeof pr_cache[s].name)) pr_cache[s].hash = pr1_hash(pr_cache[s].name);
        }
        if (!pr_cache[s].hash) continue;
        memcpy(names[s], pr_cache[s].name, 64);
        v[s].hash = pr_cache[s].hash;
        recs[s] = rec;
        for (unsigned k = 0; k < PR1_CMDS; k++) {
            uint32_t cmd = rec + PR_CMD_BASE + k * PR_CMD_SIZE, st = rd32(cmd + PR_CMD_STATUS);
            v[s].st[k] = (uint8_t)(st < 255 ? st : 255);
            if (rd32(cmd) != PR_CMD_VTBL) continue;   /* not an action command (layout differs): never forced */
            for (unsigned r = 0; r < nreg; r++)
                if (reg[r] == cmd) { v[s].act[k] = 1; break; }
        }
    }
}

static void prompt_pass(uint32_t now)
{
    if (!f_prompt || !bound) return;
    unsigned ep = w1_epoch(&N.w);
    if (ep != pr_last_epoch) pr_last_epoch = ep, pr1_reset(&PR);
    pr1_view v[PR1_SLOTS];
    char names[PR1_SLOTS][64];
    uint32_t recs[PR1_SLOTS];
    pr_read(v, names, recs);
    pr1_ev out[8];
    unsigned n = pr1_scan(&PR, v, now, out, 8);
    for (unsigned i = 0; i < n; i++) {
        uint8_t b[8];
        story_send_ext(CP_W1_PROMPT, b, pr1_enc(b, sizeof b, out[i].hash, out[i].cmd));
        const char *nm = "?";
        for (unsigned s = 0; s < PR1_SLOTS; s++) if (v[s].hash == out[i].hash) nm = names[s];
        if (++pr_logged <= 200) wlog("prompt: this PC's human pressed '%s' (action %u, hash %08x): sent to the partner", nm, out[i].cmd, (unsigned)out[i].hash);
    }
    unsigned s, k;
    while (pr1_due(&PR, v, now, &s, &k)) {
        uint32_t cmd = recs[s] + PR_CMD_BASE + k * PR_CMD_SIZE, two = PR1_PRESSED, before = rd32(cmd + PR_CMD_STATUS);
        int ok = recs[s] && rd32(cmd) == PR_CMD_VTBL && wr(cmd + PR_CMD_STATUS, &two, 4);   /* stays while the manager does not update (the 1-3 watch) */
        if (ok && H.state[WH_PCSACT] == WH_ON) pr_force_k = k, pr_force_until = GetTickCount() + 3000u, pr_force_rec = recs[s];   /* the FSM's next read says 2 */
        pr1_forced(&PR, v[s].hash, k, now);
        v[s].st[k] = PR1_PRESSED;
        if (++pr_logged <= 200)
            wlog("prompt: the partner pressed '%s' (action %u): pressed here too (slot %u, status %u -> 2%s)", names[s], k, s, (unsigned)before, ok ? "" : ": WRITE FAILED");
    }
}

static void prompt_rx(const uint8_t *b, unsigned len, uint32_t now)
{
    uint32_t h;
    unsigned k;
    if (!f_prompt || !pr1_dec(b, len, &h, &k)) return;
    int kept = pr1_remote(&PR, h, k, now);
    if (++pr_logged <= 200) wlog("prompt: the partner pressed hash %08x action %u: %s", (unsigned)h, k, kept ? "kept for this PC's copy" : "dropped (this PC's own human pressed it too, or a duplicate)");
}

static void prompt_slots(char *out, int size)   /* world pr slots: what the scan reads now (live diagnosis) */
{
    pr1_view v[PR1_SLOTS];
    char names[PR1_SLOTS][64];
    uint32_t recs[PR1_SLOTS];
    pr_read(v, names, recs);
    int o = snprintf(out, (size_t)size, "world: prompt slots (pcs manager %08x):", rd32(PR_PCSMAN));
    for (unsigned s = 0; s < PR1_SLOTS && o < size - 160; s++) {
        uint32_t mgr = rd32(PR_PCSMAN), fsm = mgr ? rd32(mgr + 0x284 + 4 * s) : 0;
        if (!fsm) continue;
        o += snprintf(out + o, (size_t)(size - o), " | %u fsm %08x rec %08x '%s' st %u%u%u%u%u%u shown %u%u%u%u%u%u", s, fsm, recs[s], names[s], v[s].st[0], v[s].st[1], v[s].st[2], v[s].st[3], v[s].st[4],
                      v[s].st[5], v[s].act[0], v[s].act[1], v[s].act[2], v[s].act[3], v[s].act[4], v[s].act[5]);
    }
}

static void prompt_status(char *out, int size)
{
    snprintf(out, (size_t)size, "world: prompt mirror %s: announced %u (echo dropped %u), received %u: pressed here %u (the FSM read it %ld), expired %u, duplicate %u, own press %u; pending %u; getter hook %s",
             f_prompt ? "ON" : "off", PR.announced, PR.echo_dropped, PR.rx, PR.applied, (long)pr_force_answered, PR.expired, PR.dup_dropped, PR.own_dropped, PR.npend, wh_state_name(H.state[WH_PCSACT]));
}
