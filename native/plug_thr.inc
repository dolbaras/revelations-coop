/*
 * P thr <ms>: which threads run the uCharaBase phase dispatchers (vtable slots 8, 9, 10 = 0x6CCAB0, 0x6C9C30, 0x6C9C10) and the uCallThink phase (0x6C9800)? A temporary counting hook
 * (removed after <ms>) counts the calls per thread id, separately for player-class units (uPl<..>), and prints the table with `P thr`. Found necessary for the pad-slot swap: if these
 * phases ran on several threads the global sGamePad+0x870 swap would be racy.
 */
#define THR_MAX 8
static struct {
    DWORD tid[THR_MAX];
    unsigned calls[THR_MAX][4], player[THR_MAX][4];
    int n;
} thr;
static hk_t hk_t8, hk_t9, hk_t10, hk_tth;
static void (FC *orig_t8)(unsigned), (FC *orig_t9)(unsigned), (FC *orig_t10)(unsigned), (FC *orig_tth)(unsigned);
static DWORD thr_until;
static volatile LONG thr_lock;

static void thr_count(int which, unsigned unit)
{
    DWORD t = GetCurrentThreadId();
    int i;
    while (InterlockedExchange(&thr_lock, 1))
        ;
    for (i = 0; i < thr.n; i++)
        if (thr.tid[i] == t)
            break;
    if (i == thr.n && thr.n < THR_MAX)
        thr.tid[thr.n++] = t;
    if (i < THR_MAX) {
        thr.calls[i][which]++;
        if (which != 3 && is_player_class(unit))
            thr.player[i][which]++;
    }
    InterlockedExchange(&thr_lock, 0);
}
static void FC hook_t8(unsigned u) { thr_count(0, u); orig_t8(u); }
static void FC hook_t9(unsigned u) { thr_count(1, u); orig_t9(u); }
static void FC hook_t10(unsigned u) { thr_count(2, u); orig_t10(u); }
static void FC hook_tth(unsigned u) { thr_count(3, u); orig_tth(u); }

static void thr_stop(void)
{
    hook_remove(&hk_t8), hook_remove(&hk_t9), hook_remove(&hk_t10), hook_remove(&hk_tth);
    thr_until = 0;
}

static void thr_report(char *out, int size)
{
    int o = snprintf(out, size, "%d threads ran the phases (calls v8/v9/v10/think, of them player-class v8/v9/v10); logic thread = ?\n", thr.n);
    for (int i = 0; i < thr.n && o + 200 < size; i++)
        o += snprintf(out + o, size - o, "tid %lu: calls %u/%u/%u/%u player %u/%u/%u\n", thr.tid[i], thr.calls[i][0], thr.calls[i][1], thr.calls[i][2], thr.calls[i][3], thr.player[i][0], thr.player[i][1], thr.player[i][2]);
}

static int thr_cmd(const char *arg, char *out, int size)
{
    static const unsigned char b_v89[] = {0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x1C, 0x0E, 0x00, 0x00}, b_v10[] = {0x8B, 0x81, 0x1C, 0x0E, 0x00, 0x00, 0xC1, 0xE8, 0x1E}, b_think[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x30};
    int ms = atoi(arg);
    if (ms <= 0) {
        thr_report(out, size);
        snprintf(out + strlen(out), size - (int)strlen(out), "game thread (this console task) = %lu", GetCurrentThreadId());
        return 1;
    }
    memset(&thr, 0, sizeof thr);
    int ok = hook_install(&hk_t8, 0x6CCAB0, b_v89, 9, (void *)hook_t8) && hook_install(&hk_t9, 0x6C9C30, b_v89, 9, (void *)hook_t9) && hook_install(&hk_t10, 0x6C9C10, b_v10, 9, (void *)hook_t10) &&
             hook_install(&hk_tth, 0x6C9800, b_think, 6, (void *)hook_tth);
    orig_t8 = (void (FC *)(unsigned))hk_t8.tramp, orig_t9 = (void (FC *)(unsigned))hk_t9.tramp, orig_t10 = (void (FC *)(unsigned))hk_t10.tramp, orig_tth = (void (FC *)(unsigned))hk_tth.tramp;
    if (!ok) {
        thr_stop();
        snprintf(out, size, "could not install the counting hooks (is the capture module holding the hook points? `cap unbind all` first)");
        return 1;
    }
    thr_until = GetTickCount() + (DWORD)ms;
    snprintf(out, size, "counting for %d ms; then `P thr` prints the table", ms);
    return 1;
}
static void thr_frame(void)
{
    if (thr_until && (int)(GetTickCount() - thr_until) >= 0)
        thr_stop();
}
