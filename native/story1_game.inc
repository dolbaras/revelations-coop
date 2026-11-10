/*
 * story1_game.inc - included by world1_game.c (uses its statics): the engine side of the Episode 1 story mirror (sf1.c; notes\ep1-world.md).
 *   E1  scenario flags: the hook of SetScenarioFlag 0x721DE0 reads the old value through the verified getter 0x721E30 and queues (id, old, new); the logic
 *       thread asks sf1: the host sends on the versioned FLAG stream, the guest sends proposals (CP_W1_SFPROP); received values go through the trampoline
 *       (no echo). The host's digest (CP_W1_SFDIG) follows the quiet window after a load.
 *   E2  the Genesis analysis rate [[0x116A84C] + 0x74] (FUN_008B9D30 is its only writer, on the scanning PC): polled before the event queue is drained; the
 *       host sends its value, the guest its rises (CP_W1_SFRATE).
 *   E6  set pieces of the verified table (uOMChainLock 0x80061600): the object lists +0xBC / +0x80 are watched every 100 ms; a piece that breaks here
 *       (E1C bit 0, the "dead" bit ArgCheckCharaState 2 reads) is announced (CP_W1_PIECE); the other PC gives its whole copy one blow through the stock
 *       ApplyHit 0x6CD270 (trampoline): the HitInfo is queued in the unit's +0xEA0 component and Damage(1) runs, so the chain's own update (uBreakChain_vf43
 *       0x626680: hp <= 0 AND a queued hit, FUN_006CB0B0) breaks it. hp 0 alone does NOT break it (e1w1b live: hp 0.0, E1C 40002080, SF 4398 stayed 0).
 * Default ON while the model is bound (a network session); `world sf off` / `world set story 0` stops it.
 */

#define WH_CSYSTEM_PTR 0x116A84Cu   /* [..] = cSystem; +0x74 = mEp12SearchRate (getter 0x7832C0) */
#define WH_RATE_OFF 0x74u

typedef unsigned (TC *flagget_fn)(void *self, unsigned id);
static sf1_t SF;
static volatile int f_story = 1;     /* the story mirror (E1 + E2 + E6) while the model is bound */
static volatile int g_sf_wide;       /* lab: every id >= 256 crosses (the hook thread reads it) */
static unsigned sf_logged, sf_logged_low, sf_apply_logged, piece_logged;
static uint32_t last_piece_ms;
static struct {
    unsigned hooked, getter_refused, local, sent, props, applied_stream, applied_prop, applied_digest, held_applied, refused_prop;
    unsigned rate_sent, rate_written, rate_unreadable;
    unsigned pieces_seen, pieces_announced, pieces_applied, pieces_pending, pieces_expired, pieces_refused;
} SS;
#define MAXPIECES 64
static struct { uint32_t k4[4]; uint32_t cls; uint8_t broken, hp0; } pcs[MAXPIECES];   /* what the watcher saw last per key */
static unsigned npcs;
static struct { uint32_t k4[4]; uint32_t cls; uint32_t since; } pend[16];          /* a remote break whose piece is not placed here (yet) */
static unsigned npend;

/* ---- engine access (logic thread unless noted) */
static int flag_get(uint32_t id)   /* any thread: the game's getter (takes the flag lock itself); -1 = unknown */
{
    uint32_t mgr = rd32(WH_FLAGMGR_PTR);
    if (!mgr || H.state[WH_C_FLAGGET] != WH_ON) return -1;
    return (int)(((flagget_fn)(uintptr_t)wh_sites[WH_C_FLAGGET].addr)((void *)(uintptr_t)mgr, id) & 0xFF);
}
static int flag_get0(void *u, uint32_t id) { (void)u; return flag_get(id) > 0; }
static void flag_apply(uint32_t id, int32_t value, const char *why)
{
    uint32_t mgr = rd32(WH_FLAGMGR_PTR);
    if (!mgr || !orig_flag) {
        S.ops_noop++;
        return;
    }
    int old = flag_get(id);
    ops_enter();
    orig_flag((void *)(uintptr_t)mgr, id, value);   /* the trampoline: no echo through our own hook */
    ops_leave();
    S.ops_flag++;
    if (++sf_apply_logged <= 3000) wlog("story: flag %u %d -> %d here (%s)", id, old, value, why);
}
static void flag_apply_cb(void *u, uint32_t id, int32_t value) { (void)u; flag_apply(id, value, "the host's digest"); SS.applied_digest++; }
static int rate_read(void)
{
    uint32_t sys = rd32(WH_CSYSTEM_PTR);
    if (!sys) return -1;
    uint32_t v = rd32(sys + WH_RATE_OFF);
    return v <= SF1_RATE_MAX ? (int)v : -1;
}
static int rate_write(int v)
{
    uint32_t sys = rd32(WH_CSYSTEM_PTR);
    int32_t x = v;
    return sys && v >= 0 && v <= SF1_RATE_MAX && wr(sys + WH_RATE_OFF, &x, 4);
}

static int story_on(void) { return f_story && bound; }

/* ---- ep1-probe item 4 (notes\ep1-world.md section 6, notes\ep1-probe.md): LAB probe of the flag store around a load, OFF unless REV1_SVPROBE=1 (from the boot,
 * so the title's profile load is seen) or `world sf sv on`. The sEventManager save-proc read (cSaveProc_vf6 0x722C20, vtable slot 0x106F140, thiscall with 3
 * arguments, ret 0xC; bytes checked) gets a POST observer by swapping the slot: the mirrored set before and after each call is logged with the caller and the
 * code addresses on its stack. The same line is logged at the restart hook's PRE (rolebarrier_on_restart: right before FUN_00720870 on this PC), at each level
 * instance start and placement of the story mirror, and around the digest. The getter 0x721E30 is called directly (its bytes are checked here, so the probe
 * works before a session). Nothing is written to the flag store. */
#define SV_SLOT 0x106F140u
#define SV_FN 0x722C20u
#define SV_GETTER 0x721E30u
static const uint8_t SV_PRO[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x4C, 0x53, 0x8B, 0xD9};
static const uint8_t SV_GET_PRO[17] = {0x56, 0x8B, 0xF1, 0x57, 0x85, 0xF6, 0x74, 0x05, 0xE8, 0x83, 0x3B, 0x2B, 0x00, 0x8B, 0x7C, 0x24, 0x0C};
typedef unsigned (TC *svread_fn)(void *self, void *a, void *b, void *c);
void rolebarrier_on_restart(void (*cb)(void));   /* rolebarrier1_game.c: called on the game thread right before FUN_00720870 */
static volatile int sv_state;        /* 0 off, 1 the slot is ours, -1 refused (bytes) */
static volatile LONG sv_calls;
static unsigned sv_lines;
static int sv_bytes_ok(uint32_t a, const uint8_t *p, unsigned n)
{
    uint8_t b[32];
    return n <= sizeof b && r1_safe_read((const void *)(uintptr_t)a, b, n) && !memcmp(b, p, n);
}
static int sv_get(void *u, uint32_t id)
{
    (void)u;
    uint32_t mgr = rd32(WH_FLAGMGR_PTR);
    if (!mgr || !rd32(mgr + 0x34) || (int)rd32(mgr + 0x38) <= 0) return -1;   /* no store yet (boot) */
    return (int)(((flagget_fn)(uintptr_t)SV_GETTER)((void *)(uintptr_t)mgr, id) & 0xFF);
}
static void sv_note(const char *what)   /* one probe line: what, count, hash, the set mirrored ids (logic thread or the loading thread) */
{
    if (sv_state != 1 || ++sv_lines > 400) return;
    char ids[1400];
    uint32_t h = 0;
    int n = sf1_set_summary(sv_get, NULL, &h, ids, sizeof ids);
    if (n < 0) r1_log("world: story: SVPROBE %s: the flag store is not there (manager %08x)", what, rd32(WH_FLAGMGR_PTR));
    else r1_log("world: story: SVPROBE %s: mirrored set %d, hash %08x: %s", what, n, (unsigned)h, ids);
}
static unsigned TC sv_read_hook(void *self, void *a, void *b, void *c)
{
    LONG k = InterlockedIncrement(&sv_calls);
    uint32_t ret = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t stk[6] = {0}, *sp = (uint32_t *)__builtin_frame_address(0);
    for (int i = 0, m = 0; i < 64 && m < 6; i++) {   /* code addresses on the caller's stack: the call chain, roughly */
        uint32_t v = 0;
        if (r1_safe_read(sp + i, &v, 4) && v >= 0x401000u && v < 0xF00000u && v != ret) stk[m++] = v;
    }
    char w[200];
    snprintf(w, sizeof w, "save read #%ld BEFORE (this %08x, args %08x %08x %08x, caller %08x, stack %08x %08x %08x %08x %08x %08x)", (long)k, (unsigned)(uintptr_t)self, (unsigned)(uintptr_t)a,
             (unsigned)(uintptr_t)b, (unsigned)(uintptr_t)c, (unsigned)ret, stk[0], stk[1], stk[2], stk[3], stk[4], stk[5]);
    sv_note(w);
    unsigned r = ((svread_fn)(uintptr_t)SV_FN)(self, a, b, c);
    snprintf(w, sizeof w, "save read #%ld AFTER (returned %08x)", (long)k, r);
    sv_note(w);
    return r;
}
static void sv_restart_cb(void) { sv_note("restart hook PRE (FUN_00720870 runs next on this PC)"); }
static void sv_set(int on, char *out, int size)
{
    uint32_t cur = rd32(SV_SLOT), mine = (uint32_t)(uintptr_t)sv_read_hook;
    if (on && sv_state != 1) {
        if (cur != SV_FN || !sv_bytes_ok(SV_FN, SV_PRO, sizeof SV_PRO) || !sv_bytes_ok(SV_GETTER, SV_GET_PRO, sizeof SV_GET_PRO) || !m_write(NULL, SV_SLOT, &mine, 4)) {
            sv_state = -1;
            if (out) snprintf(out, (size_t)size, "world: SVPROBE REFUSED: slot %08x holds %08x (want %08x), prologue %s, getter %s", SV_SLOT, (unsigned)cur, SV_FN,
                              sv_bytes_ok(SV_FN, SV_PRO, sizeof SV_PRO) ? "ok" : "DIFFERS", sv_bytes_ok(SV_GETTER, SV_GET_PRO, sizeof SV_GET_PRO) ? "ok" : "DIFFERS");
            r1_log("world: story: SVPROBE refused (slot %08x = %08x)", SV_SLOT, (unsigned)cur);
            return;
        }
        sv_state = 1;
        r1_log("world: story: SVPROBE ON: the save-proc read slot %08x now %08x (was %08x)", SV_SLOT, (unsigned)mine, (unsigned)cur);
    } else if (!on && sv_state == 1) {
        uint32_t orig = SV_FN;
        if (cur == mine) m_write(NULL, SV_SLOT, &orig, 4);
        sv_state = 0;
        r1_log("world: story: SVPROBE off (slot %08x = %08x)", SV_SLOT, rd32(SV_SLOT));
    }
    if (out) snprintf(out, (size_t)size, "world: SVPROBE %s (slot %08x = %08x), save reads seen %ld, lines %u", sv_state == 1 ? "ON" : sv_state < 0 ? "REFUSED" : "off", SV_SLOT, rd32(SV_SLOT),
                      (long)sv_calls, sv_lines);
}
/* ---- ep1-probe: the pre-load digest (sf1_pre_*): the host's mirrored bitmap at its restart PRE, written into the guest's store at the guest's restart PRE */
static sf1_pre_t SPRE;
static uint32_t spre_seq;
static volatile int f_spre = 1;   /* `world sf pre on|off` (lab) */
static int spre_get(void *u, uint32_t id) { (void)u; return flag_get(id); }   /* -1 = the getter is not verified: that id is left alone */
static void spre_set(void *u, uint32_t id, int32_t v) { (void)u; flag_apply(id, v, "the host's pre-load digest"); }
static void spre_restart_cb(void)   /* game thread, right before FUN_00720870 on this PC */
{
    EnterCriticalSection(&mcs);
    if (bound && f_story && f_spre) {
        if (bound_role == W1_HOST) {
            uint8_t b[SF1_PRE_LEN];
            unsigned n = H.state[WH_C_FLAGGET] == WH_ON ? sf1_pre_build(spre_get, NULL, ++spre_seq, b, sizeof b) : 0;
            int ok = n && cs_send_event(bound_cs, CP_EV_SFPRE, b, n);
            if (ok) SPRE.tx++;
            unsigned set = 0;
            for (unsigned i = 0; i < SF1_BYTES; i++) set += (unsigned)__builtin_popcount(b[6 + i]);
            wlog("story: pre-load digest #%u %s at this host's restart (%u mirrored flags set here)", (unsigned)spre_seq, ok ? "sent" : n ? "NOT sent (no session)" : "NOT built (the flag getter is not verified)", n ? set : 0);
        } else {
            unsigned s = 0, c = 0;
            uint32_t age = SPRE.have ? (uint32_t)(cs_now(bound_cs) - SPRE.at) : 0;
            if (sf1_pre_apply(&SPRE, cs_now(bound_cs), spre_get, spre_set, NULL, &s, &c)) {
                wlog("story: the host's pre-load digest #%u (%u ms old) applied before this PC's load: set here %u, cleared here %u", (unsigned)SPRE.seq, (unsigned)age, s, c);
                sv_note("after the host's pre-load digest");
            } else
                wlog("story: no fresh pre-load digest of the host at this PC's restart (stale %u, applied before %u): the post-load digest checks", SPRE.stale, SPRE.applied);
        }
    }
    LeaveCriticalSection(&mcs);
}
static void story_pre_rx(const uint8_t *b, unsigned n)   /* the note tap (under mcs): the guest keeps the newest */
{
    if (!bound || bound_role == W1_HOST) return;
    if (sf1_pre_take(&SPRE, b, n, cs_now(bound_cs))) wlog("story: the host's pre-load digest #%u received (applied at this PC's restart)", (unsigned)SPRE.seq);
}

static void sv_init(void)
{
    char v[8];
    rolebarrier_on_restart(sv_restart_cb);   /* silent unless the probe is ON */
    rolebarrier_on_restart(spre_restart_cb);   /* the pre-load digest (product, in a session) */
    if (r1_env("REV1_SVPROBE", v, sizeof v) && v[0] == '1') sv_set(1, NULL, 0);
}

static void story_send_ext(unsigned sub, const uint8_t *b, unsigned n)
{
    if (!n || !w1_send_ext(&N.w, sub, b, n)) wlog("story: ext %u (%u bytes) NOT queued", sub, n);
}
static void story_send_prop(void *u, uint32_t id)
{
    (void)u;
    uint8_t b[8];
    story_send_ext(CP_W1_SFPROP, b, sf1_enc_prop(b, sizeof b, id, 1));
    SS.props++;
    if (++sf_logged <= 3000) wlog("story: flag %u = 1 (own, set in the load window): proposed to the host", id);
}
static void story_send_rate(int kind, int v)
{
    uint8_t b[4];
    story_send_ext(CP_W1_SFRATE, b, sf1_enc_rate(b, sizeof b, kind, v));
    SS.rate_sent++;
    wlog("story: Genesis rate %s %d sent", kind == SF1_RATE_ABS ? "value" : "rise", v);
}

/* ---- the hook side (game threads): the old value before the store. Only ids that may cross are read (the getter takes the flag lock) */
static int story_hook_old(uint32_t id)
{
    if (!g_active || !f_story || in_ops()) return -2;   /* not reported */
    if (!(sf1_index(id) >= 0 || (g_sf_wide && id >= 256 && id < 0x10000))) return -3;   /* reported for the log only */
    int old = flag_get(id);
    if (old < 0) SS.getter_refused++;
    SS.hooked++;
    return old;
}

/* ---- logic thread */
static void story_bind(void)
{
    sf1_init(&SF, bound_role == W1_HOST ? SF1_HOST : SF1_GUEST);
    SF.wide = g_sf_wide;
    w1_set_flag_raw(&N.w, 1);
    npcs = npend = 0;
}

/* this PC's own human is in the level: its unit exists and stands somewhere other than 0,0,0 (e1w2 live: after the restart call the units exist at 0,0,0
 * for the whole 1-1 start movie, ~110 s, while the session's barrier is already open) */
static int own_placed(void)
{
    uint32_t u = 0;
    if (!wh_player_by_slot(&MEM, (unsigned)g_own, &u) || !u) return 0;
    float x = rdf(u + WH_U_POS), y = rdf(u + WH_U_POS + 4), z = rdf(u + WH_U_POS + 8);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return 0;
    return fabsf(x) + fabsf(y) + fabsf(z) > 1.0f;
}

static void story_epoch(uint32_t now)
{
    unsigned ep = w1_epoch(&N.w);
    if (ep == 0xFFFFFFFFu) return;
    if (!SF.epoch_valid || SF.epoch != ep) {
        SF.wide = g_sf_wide;
        sf1_epoch(&SF, ep, now);
        npcs = npend = 0;
        int placed = own_placed();
        if (!placed) sf1_unplaced(&SF);
        wlog("story: level instance %u: quiet window %u ms %s, then %s", ep, SF1_QUIET_MS, placed ? "from now" : "from the moment this PC's human is placed (not yet: a movie / the load)",
             SF.role == SF1_HOST ? "the digest goes to the guest" : "the host's digest is applied");
        char w[64];
        snprintf(w, sizeof w, "level instance %u begins (%s)", ep, placed ? "placed" : "not placed yet");
        sv_note(w);
    }
    if (SF.unplaced && own_placed() && sf1_placed(&SF, now)) {
        wlog("story: this PC's human is placed %u ms after the level instance began: the quiet window (%u ms) starts now", (unsigned)(now - SF.epoch_ms), SF1_QUIET_MS);
        sv_note("this PC's human is placed");
    }
}

/* drain: an own change of the engine (old -1 = unknown, -3 = not crossing: logged only) */
static void story_local(uint32_t id, int old, int32_t value)
{
    if (old == -3) {
        if (++sf_logged_low <= 200) wlog("scenario flag %u = %d (local, not mirrored)", id, value);
        return;
    }
    if (!story_on() || old == -2) return;
    SS.local++;
    int why = 0;
    int d = sf1_local(&SF, id, old, value, cs_now(bound_cs), &why);
    if (d == SF1_SEND) {
        if (SF.role == SF1_HOST) {
            if (w1_local_flag(&N.w, id, value)) S.flags_sent++, SS.sent++;
        } else {
            uint8_t b[8];
            story_send_ext(CP_W1_SFPROP, b, sf1_enc_prop(b, sizeof b, id, value));
            SS.props++;
        }
    }
    if (++sf_logged <= 3000)
        wlog("story: flag %u %d -> %d (own): %s%s", id, old, value, d == SF1_SEND ? (SF.role == SF1_HOST ? "sent to the guest" : "proposed to the host") : "kept here: ", d == SF1_SEND ? "" : sf1_why_name(why));
}

/* the model: a FLAG of the stream (host -> guest); on the host = an older guest's FLAG, handled as a proposal */
static void story_op_flag(void *u, uint32_t id, int32_t value)
{
    (void)u;
    if (!story_on()) return;
    int why = 0, cur = flag_get(id);
    if (SF.role == SF1_HOST) {
        if (sf1_proposal(&SF, id, value, cur > 0 ? cur : 0, &why) == SF1_APPLY) flag_apply(id, value, "the guest's FLAG"), w1_note_flag(&N.w, id, value), SS.applied_prop++;
        else SS.refused_prop++;
        return;
    }
    int d = sf1_remote_stream(&SF, id, value, cur < 0 ? !value : cur, cs_now(bound_cs), &why);
    if (d == SF1_APPLY) flag_apply(id, value, "from the host"), SS.applied_stream++;
    else if (d == SF1_LATER) {
        if (++sf_apply_logged <= 3000) wlog("story: flag %u = %d from the host: held %u ms (its set must be seen here first)", id, value, SF1_HOLD_MS);
    }
}

static void piece_apply_here(const uint32_t k4[4], uint32_t cls, uint32_t now, int first);
static void prompt_rx(const uint8_t *b, unsigned len, uint32_t now);   /* prompt1_game.inc (E8) */
static void local_rx(const uint8_t *b, unsigned len);                 /* local1_game.inc (rev1 live 11) */
static void stage_rx(const uint8_t *b, unsigned len);                 /* local1_game.inc (rev1 live 12) */
static void watch_rx(const uint8_t *b, unsigned len);                 /* watch1_game.inc (rev1 live 13) */
static void hitx_rx(const uint8_t *b, unsigned len);                  /* hitx1_game.inc (rev1 live 14) */
static void watchcam_rx(const uint8_t *b, unsigned len);              /* watch1_game.inc (rev1 live 14) */
static void motion_rx(const uint8_t *b, unsigned len);                /* motion1_game.inc (rev1 live 14) */
static void bag_rx(const uint8_t *b, unsigned len);                   /* bag1_game.inc (rev1 live 14) */

static void op_ext(void *u, unsigned sub, const uint8_t *b, unsigned len)
{
    (void)u;
    if (sub == CP_W1_ZONE) {   /* round 9: the partner's own human in / out of a story zone (zone1_game.inc) */
        if (bound) zone_rx(b, len);
        return;
    }
    if (sub == CP_W1_LOCAL) {   /* rev1 live 11: the client's report of an enemy the host does not simulate (local1_game.inc) */
        if (bound) local_rx(b, len);
        return;
    }
    if (sub == CP_W1_STAGE) {   /* rev1 live 12: the stage part codes under the partner's active human (stage1.h, local1_game.inc) */
        if (bound) stage_rx(b, len);
        return;
    }
    if (sub == CP_W1_RLOC) {   /* rev1 live 13: the records of the enemies run on the client's AI (watch1.h, watch1_game.inc) */
        if (bound) watch_rx(b, len);
        return;
    }
    if (sub == CP_W1_HITX) {   /* rev1 live 14: the client's blow with its real HitInfo (hitx1.h, hitx1_game.inc) */
        if (bound) hitx_rx(b, len);
        return;
    }
    if (sub == CP_W1_CAM) {   /* rev1 live 14: the active PC's camera for the watching PC (watchcam1.h, watch1_game.inc) */
        if (bound) watchcam_rx(b, len);
        return;
    }
    if (sub == CP_W1_MOT) {   /* rev1 live 14: an own-AI enemy's motion for the other PC's replica (mot1.h, motion1_game.inc) */
        if (bound) motion_rx(b, len);
        return;
    }
    if (sub == CP_W1_BAG) {   /* rev1 live 14: the partner's guns for his puppet here (bag1.h, bag1_game.inc) */
        if (bound) bag_rx(b, len);
        return;
    }
    if (!story_on()) return;
    int why = 0;
    uint32_t now = cs_now(bound_cs);
    if (sub == CP_W1_SFPROP) {
        uint32_t id;
        int32_t v;
        if (!sf1_dec_prop(b, len, &id, &v)) return;
        int cur = flag_get(id);
        if (sf1_proposal(&SF, id, v, cur > 0 ? cur : 0, &why) == SF1_APPLY) {
            flag_apply(id, v, "the guest's proposal");
            w1_note_flag(&N.w, id, v);   /* the host's table: its later clear is newer; not sent (the guest has it) */
            SS.applied_prop++;
        } else {
            SS.refused_prop++;
            if (++sf_apply_logged <= 3000) wlog("story: the guest's flag %u = %d refused here (%s; here %d)", id, v, sf1_why_name(why), cur);
        }
    } else if (sub == CP_W1_SFDIG) {
        unsigned set0 = SF.st.digest_set, clr0 = SF.st.digest_clear, prop0 = SF.st.digest_prop;
        int ok = sf1_digest_apply(&SF, b, len, flag_get0, flag_apply_cb, story_send_prop, NULL, now);
        wlog("story: the host's digest (%u bytes) %s: set here %u, cleared here %u, own sets proposed %u", len, ok == 1 ? "applied" : ok == 2 ? "kept until this PC's own quiet window ends" : "REFUSED (layout / length)",
             SF.st.digest_set - set0, SF.st.digest_clear - clr0, SF.st.digest_prop - prop0);
        if (ok == 1) sv_note("after the host's digest");
    } else if (sub == CP_W1_SFRATE) {
        int kind, v, o, send, cur = rate_read();
        if (cur < 0) {
            SS.rate_unreadable++;
            return;
        }
        if (sf1_dec_rate(b, len, &kind, &v) && sf1_rate_remote(&SF, kind, v, cur, &o, &send)) {
            int ok = rate_write(o);
            SS.rate_written += ok;
            wlog("story: Genesis rate %d -> %d here (%s %d from the other PC)%s", cur, o, kind == SF1_RATE_ABS ? "value" : "rise", v, ok ? "" : ": WRITE FAILED");
            if (send) story_send_rate(SF1_RATE_ABS, o);
        }
    } else if (sub == CP_W1_PROMPT) {
        prompt_rx(b, len, now);
    } else if (sub == CP_W1_PIECE) {
        uint32_t k[4], cls;
        if (sf1_dec_piece(b, len, k, &cls) && sf1_piece_remote(&SF, k, cls)) piece_apply_here(k, cls, now, 1);
    }
}

/* before the event queue: the rate (a scan raises it in the same frame that sets its flag) */
static void story_pre_drain(void)
{
    if (!story_on() || !SF.epoch_valid) return;
    int cur = rate_read(), kind, v;
    if (cur >= 0 && sf1_rate_poll(&SF, cur, &kind, &v)) story_send_rate(kind, v);
}

/* ---- set pieces */
static int piece_broken(uint32_t unit) { return (rd32(unit + WH_U_FLAGS) & 1u) != 0; }   /* the dead bit (ArgCheckCharaState 2); hp <= 0 alone is not a break */

/* one blow on a whole piece through the stock ApplyHit (trampoline): as the partner's puppet's replayed blow would land here. The attacker is the partner's
 * puppet (a player unit of kind 1 on this PC) when there is one, else none; byte +0x58 bit 0x20 is what uBreakChain_vf45 0x626150 asks of a blow. */
static unsigned piece_blow(uint32_t unit, const char *why)
{
    uint32_t att = 0;
    if (!orig_apply || H.state[WH_APPLYHIT] != WH_ON) {
        wlog("story: piece unit %08x: no blow (%s): ApplyHit hook %s", (unsigned)unit, why, wh_state_name(H.state[WH_APPLYHIT]));
        return 0;
    }
    if (wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &att)) {
        uint32_t k = rd32(att + WH_U_KIND);
        if (k == 0 || k == 3) att = 0;   /* ApplyHit refuses an owner of kind 0 / 3 */
    }
    float pt[3] = {rdf(unit + WH_U_POS), rdf(unit + WH_U_POS + 4), rdf(unit + WH_U_POS + 8)};
    uint8_t hit[WH_HIT_SIZE];
    wh_hit_build(hit, WH_HIT_VTBL, att, att, 0, pt);
    hit[0x58] |= 0x20;
    float hp0 = rdf(unit + WH_U_HP);
    ops_enter();
    unsigned r = orig_apply((void *)(uintptr_t)unit, hit);
    ops_leave();
    wlog("story: piece unit %08x (%s): one blow through ApplyHit (attacker %08x): returned %u, hp %.1f -> %.1f, hit queue %u, E1C %08x", (unsigned)unit, why, (unsigned)att, r,
         (double)hp0, (double)rdf(unit + WH_U_HP), rd32(rd32(unit + 0xEA0) + 0x14), rd32(unit + WH_U_FLAGS));
    return r;
}

/* the chain is placed by ArgCharaSet (list 1, data 10): it is looked for in the object list +0xBC and in the character list +0x80 */
static const uint32_t PIECE_LISTS[2] = {WH_LIST_OBJECTS, WH_LIST_ENEMIES};
static int piece_find_unit(const uint32_t k4[4], uint32_t *unit)
{
    uint32_t sa = sarrange();
    wh_entry e;
    for (int l = 0; sa && l < 2; l++)
        if (wh_find_key4(&MEM, sa, PIECE_LISTS[l], k4, &e) && e.unit) {
            *unit = e.unit;
            return 1;
        }
    return 0;
}

static void piece_apply_here(const uint32_t k4[4], uint32_t cls, uint32_t now, int first)
{
    uint32_t unit = 0;
    if (!piece_find_unit(k4, &unit) || wh_unit_class(&MEM, unit) != cls) {
        if (first && npend < 16) {
            memcpy(pend[npend].k4, k4, sizeof pend[npend].k4), pend[npend].cls = cls, pend[npend].since = now, npend++;
            SS.pieces_pending++;
            wlog("story: piece %08x key %08x/%08x/%u/%u broken by the partner: not placed here yet, kept", (unsigned)cls, (unsigned)k4[0], (unsigned)k4[1], (unsigned)k4[2], (unsigned)k4[3]);
        }
        return;
    }
    float hp = rdf(unit + WH_U_HP);
    if (piece_broken(unit)) {
        wlog("story: piece unit %08x class %08x broken by the partner: already broken here (hp %.1f, E1C %08x)", (unsigned)unit, (unsigned)cls, (double)hp, rd32(unit + WH_U_FLAGS));
        return;
    }
    wlog("story: piece unit %08x class %08x key %08x/%08x/%u/%u broken by the partner: whole here (hp %.1f): one blow", (unsigned)unit, (unsigned)cls, (unsigned)k4[0], (unsigned)k4[1], (unsigned)k4[2],
         (unsigned)k4[3], (double)hp);
    if (piece_blow(unit, "the partner broke it")) SS.pieces_applied++;
    else SS.pieces_refused++;
}

static void piece_pass(uint32_t now)
{
    if ((int32_t)(now - last_piece_ms) < 100) return;
    last_piece_ms = now;
    for (unsigned i = 0; i < npend;) {   /* remote breaks waiting for their piece */
        uint32_t unit = 0;
        if (piece_find_unit(pend[i].k4, &unit)) {
            piece_apply_here(pend[i].k4, pend[i].cls, now, 0);
            pend[i] = pend[--npend];
        } else if ((uint32_t)(now - pend[i].since) > 120000u) {
            SS.pieces_expired++;
            pend[i] = pend[--npend];
        } else
            i++;
    }
    uint32_t sa = sarrange();
    if (!sa) return;
    for (int l = 0; l < 2; l++) {
    unsigned n = wh_list_count(&MEM, sa, PIECE_LISTS[l]);
    for (unsigned i = 0; i < n; i++) {
        wh_entry e;
        if (!wh_list_at(&MEM, sa, PIECE_LISTS[l], i, &e) || !e.unit) continue;
        uint32_t cls = wh_unit_class(&MEM, e.unit);
        if (!sf1_piece_class(cls)) continue;
        int br = piece_broken(e.unit);
        unsigned p;
        for (p = 0; p < npcs && memcmp(pcs[p].k4, e.k, sizeof e.k); p++) {}
        if (p == npcs) {   /* first sight: the baseline (a piece already broken at the load is never announced) */
            if (npcs >= MAXPIECES) continue;
            memcpy(pcs[npcs].k4, e.k, sizeof e.k), pcs[npcs].cls = cls, pcs[npcs].broken = (uint8_t)br, pcs[npcs].hp0 = 0, npcs++;
            SS.pieces_seen++;
            if (++piece_logged <= 40)
                wlog("story: piece unit %08x class %08x (list +0x%X) key %08x/%08x/%u/%u seen: hp %.1f E1C %08x (%s)", (unsigned)e.unit, (unsigned)cls, (unsigned)PIECE_LISTS[l], (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2], (unsigned)e.k[3],
                     (double)rdf(e.unit + WH_U_HP), rd32(e.unit + WH_U_FLAGS), br ? "broken" : "whole");
            continue;
        }
        if (!br && !pcs[p].hp0 && !(rdf(e.unit + WH_U_HP) > 0.0f)) {   /* diagnosis only: hp 0 without the dead bit is not a break (e1w1b) */
            pcs[p].hp0 = 1;
            wlog("story: piece unit %08x class %08x: hp 0 here, not broken (E1C %08x, hit queue %u)", (unsigned)e.unit, (unsigned)cls, rd32(e.unit + WH_U_FLAGS), rd32(rd32(e.unit + 0xEA0) + 0x14));
        }
        if (pcs[p].broken || !br) continue;
        pcs[p].broken = 1;
        if (sf1_piece_local(&SF, e.k, cls)) {
            uint8_t b[20];
            story_send_ext(CP_W1_PIECE, b, sf1_enc_piece(b, sizeof b, e.k, cls));
            SS.pieces_announced++;
            wlog("story: piece unit %08x class %08x key %08x/%08x/%u/%u broken here (hp %.1f, E1C %08x): announced to the partner", (unsigned)e.unit, (unsigned)cls, (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2],
                 (unsigned)e.k[3], (double)rdf(e.unit + WH_U_HP), rd32(e.unit + WH_U_FLAGS));
        } else
            wlog("story: piece unit %08x class %08x broken here by the partner's announcement (no echo)", (unsigned)e.unit, (unsigned)cls);
    }
    }
}

/* after the model's frame, while PLAYING */
static void story_pass(uint32_t now)
{
    if (!story_on() || !SF.epoch_valid) return;
    if (SF.role == SF1_HOST && sf1_digest_due(&SF, now)) {
        uint8_t b[W1_EXT_MAX];
        unsigned n = H.state[WH_C_FLAGGET] == WH_ON ? sf1_digest_build(&SF, flag_get0, NULL, b, sizeof b) : 0;
        unsigned set = 0;
        for (unsigned i = 0; i < n && i + 4 < n; i++) set += (unsigned)__builtin_popcount(b[4 + i]);
        if (n) story_send_ext(CP_W1_SFDIG, b, n);
        wlog("story: digest after the quiet window %s (%u mirrored flags set here)%s", n ? "sent" : "NOT sent (the flag getter is not verified)", set, "");
        sv_note("the digest this host sent");
        int rv = sf1_rate_value(&SF);
        if (rv >= 0) story_send_rate(SF1_RATE_ABS, rv);
    }
    if (SF.role == SF1_GUEST) {
        unsigned set0 = SF.st.digest_set, clr0 = SF.st.digest_clear, prop0 = SF.st.digest_prop;
        if (sf1_digest_kept(&SF, now, flag_get0, flag_apply_cb, story_send_prop, NULL))
            wlog("story: the kept digest applied after this PC's quiet window: set here %u, cleared here %u, own sets proposed %u", SF.st.digest_set - set0, SF.st.digest_clear - clr0, SF.st.digest_prop - prop0), sv_note("after the host's kept digest");
        if (sf1_digest_timeout(&SF, now, flag_get0, story_send_prop, NULL)) wlog("story: no digest from the host %u ms after the quiet window: own sets go on as proposals", SF1_DIGEST_WAIT_MS);
        uint32_t id;
        int32_t v;
        while (sf1_tick(&SF, now, &id, &v)) flag_apply(id, v, "the host's held clear"), SS.held_applied++;
    }
    piece_pass(now);
}

static void story_status(char *out, int size)
{
    char sum[900];
    sf1_summary(&SF, sum, sizeof sum);
    snprintf(out, (size_t)size,
             "story mirror %s (world set story 0|1; wide %d), flag getter %s, rate here %d:\n  %s\n  glue: hooked %u (getter refused %u), own changes %u, stream sent %u, proposals %u; applied: stream %u, proposals %u, digest %u, held %u; proposals refused %u; "
             "rate sent %u written %u (unreadable %u); pieces seen %u announced %u applied %u (pending %u, expired %u, refused %u)",
             f_story ? "ON" : "off", g_sf_wide, wh_state_name(H.state[WH_C_FLAGGET]), rate_read(), sum, SS.hooked, SS.getter_refused, SS.local, SS.sent, SS.props, SS.applied_stream, SS.applied_prop, SS.applied_digest, SS.held_applied,
             SS.refused_prop, SS.rate_sent, SS.rate_written, SS.rate_unreadable, SS.pieces_seen, SS.pieces_announced, SS.pieces_applied, SS.pieces_pending, SS.pieces_expired, SS.pieces_refused);
    size_t o = strlen(out);
    for (unsigned p = 0; p < npcs && o + 120 < (size_t)size; p++) {
        uint32_t unit = 0;
        piece_find_unit(pcs[p].k4, &unit);
        o += (size_t)snprintf(out + o, (size_t)size - o, "\n  piece %08x key %08x/%08x/%u/%u unit %08x hp %.1f E1C %08x %s (mirror state %d)", (unsigned)pcs[p].cls, (unsigned)pcs[p].k4[0], (unsigned)pcs[p].k4[1],
                              (unsigned)pcs[p].k4[2], (unsigned)pcs[p].k4[3], (unsigned)unit, unit ? (double)rdf(unit + WH_U_HP) : 0.0, unit ? rd32(unit + WH_U_FLAGS) : 0, pcs[p].broken ? "broken" : "whole", sf1_piece_known(&SF, pcs[p].k4));
    }
}

/* world sf on|off | wide 0|1 | get <id> | set <id> <v> (LAB: through the game's own SetScenarioFlag, i.e. as if a script did it) | rate [set <v>] (LAB) */
typedef unsigned (TC *flagset_fn)(void *self, unsigned id, int value);
static void story_cmd(const char *a2, const char *a3, const char *a4, char *out, int size)
{
    if (!a2[0] || !strcmp(a2, "status")) {
        story_status(out, size);
    } else if (!strcmp(a2, "on") || !strcmp(a2, "off")) {
        f_story = !strcmp(a2, "on");
        snprintf(out, (size_t)size, "world: story mirror %s", f_story ? "ON" : "off");
    } else if (!strcmp(a2, "wide") && (a3[0] == '0' || a3[0] == '1')) {
        g_sf_wide = a3[0] == '1';
        SF.wide = g_sf_wide;
        snprintf(out, (size_t)size, "world: story mirror wide = %d (%s)", g_sf_wide, g_sf_wide ? "LAB: every flag id >= 256 crosses" : "the Episode 1 whitelist");
    } else if (!strcmp(a2, "get") && a3[0]) {
        uint32_t id = (uint32_t)strtoul(a3, NULL, 0);
        snprintf(out, (size_t)size, "world: flag %u = %d here (mirrored %s)", id, flag_get(id), sf1_index(id) >= 0 ? "yes" : "no");
    } else if (!strcmp(a2, "set") && a3[0] && a4[0]) {
        uint32_t id = (uint32_t)strtoul(a3, NULL, 0), mgr = rd32(WH_FLAGMGR_PTR);
        int v = atoi(a4);
        if (!mgr || H.state[WH_FLAG] != WH_ON) snprintf(out, (size_t)size, "world: flag set refused (manager %08x, hook %s)", (unsigned)mgr, wh_state_name(H.state[WH_FLAG]));
        else {
            int old = flag_get(id);
            LeaveCriticalSection(&mcs);   /* the hook queues; the drain needs no lock here, but the store must not run under the model lock */
            ((flagset_fn)(uintptr_t)wh_sites[WH_FLAG].addr)((void *)(uintptr_t)mgr, id, v);
            EnterCriticalSection(&mcs);
            snprintf(out, (size_t)size, "world: LAB flag %u %d -> %d through SetScenarioFlag (as a script would)", id, old, flag_get(id));
        }
    } else if (!strcmp(a2, "list")) {   /* the mirrored flags set here (for the per-step compare of a pair run) */
        size_t o = 0;
        unsigned n = 0;
        int unk = 0;
        o += (size_t)snprintf(out, (size_t)size, "world: mirrored flags set here:");
        for (unsigned i = 0; i < SF1_N; i++) {
            int v = flag_get(sf1_id_at(i));
            if (v < 0) unk++;
            if (v > 0) {
                n++;
                if (o + 12 < (size_t)size) o += (size_t)snprintf(out + o, (size_t)size - o, " %u", sf1_id_at(i));
            }
        }
        if (o + 40 < (size_t)size) snprintf(out + o, (size_t)size - o, " | count %u%s", n, unk ? " (getter NOT verified)" : "");
    } else if (!strcmp(a2, "break") && a3[0]) {   /* LAB: the applier's blow on this PC (as if the partner had announced the break) */
        uint32_t unit = (uint32_t)strtoul(a3, NULL, 16);
        if (!unit || !sf1_piece_class(wh_unit_class(&MEM, unit))) snprintf(out, (size_t)size, "world: %08x is not a piece of the table (class %08x)", (unsigned)unit, unit ? (unsigned)wh_unit_class(&MEM, unit) : 0);
        else {
            unsigned r = piece_blow(unit, "lab");
            snprintf(out, (size_t)size, "world: LAB blow on piece %08x: ApplyHit returned %u, hp %.1f, E1C %08x (the break follows in its own update)", (unsigned)unit, r, (double)rdf(unit + WH_U_HP), rd32(unit + WH_U_FLAGS));
        }
    } else if (!strcmp(a2, "pre")) {   /* the pre-load digest: world sf pre [on|off] */
        if (!strcmp(a3, "on") || !strcmp(a3, "off")) f_spre = !strcmp(a3, "on");
        snprintf(out, (size_t)size, "world: pre-load digest %s: sent %u, received %u (bad %u), applied %u (set %u, cleared %u), stale %u%s", f_spre ? "ON" : "off", SPRE.tx, SPRE.rx, SPRE.bad, SPRE.applied,
                 SPRE.set_n, SPRE.clr_n, SPRE.stale, SPRE.have ? ", one pending" : "");
    } else if (!strcmp(a2, "sv")) {   /* LAB (ep1-probe): the save-flag probe: world sf sv [on|off|now] */
        if (!strcmp(a3, "on") || !strcmp(a3, "off")) sv_set(!strcmp(a3, "on"), out, size);
        else if (!strcmp(a3, "now")) {
            int was = sv_state;
            if (was != 1) sv_state = 1;
            sv_note("console `world sf sv now`");
            sv_state = was;
            snprintf(out, (size_t)size, "world: SVPROBE line written to the log");
        } else sv_set(sv_state == 1, out, size);
    } else if (!strcmp(a2, "rate")) {
        if (!strcmp(a3, "set") && a4[0]) {
            int v = atoi(a4);
            snprintf(out, (size_t)size, "world: LAB Genesis rate %d -> %d %s", rate_read(), v, rate_write(v) ? "written (as a scan would)" : "NOT written");
        } else
            snprintf(out, (size_t)size, "world: Genesis rate here %d (cSystem %08x)", rate_read(), rd32(WH_CSYSTEM_PTR));
    } else {
        snprintf(out, (size_t)size, "world sf [status] | on | off | wide 0|1 | list | get <id> | set <id> <v> (lab) | break <unit> (lab) | rate [set <v>] (lab) | sv [on|off|now] (lab probe)");
    }
}
