/* Unit tests of the wire protocol (coop_proto.c): round trips, byte vectors, truncation, trailing bytes, fuzz. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/coop_proto.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(const uint8_t *b, size_t n, char *o)
{
    for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]);
}

static void sample_session(cp_session *s)
{
    memset(s, 0, sizeof *s);
    s->id = 0xA1B2C3D4u, s->game_type = 0, s->episode = 2, s->part = 1, s->difficulty = 3, s->max_players = 2, s->flags = 5, s->area_id = 100, s->seed = 0x01020304u;
}
static void sample_lobby(cp_lobby *l)
{
    memset(l, 0, sizeof *l);
    l->rev = 7, l->count = 2;
    l->p[0].pid = 0, l->p[0].character = CP_CHAR_CLAIRE, l->p[0].ready = 1, l->p[0].flags = CP_PF_HOST | CP_PF_CONNECTED;
    snprintf(l->p[0].name, sizeof l->p[0].name, "Host");
    l->p[1].pid = 1, l->p[1].character = CP_CHAR_MOIRA, l->p[1].ready = 0, l->p[1].flags = CP_PF_CONNECTED | CP_PF_BOT;
    snprintf(l->p[1].name, sizeof l->p[1].name, "\xD0\x98\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA");   /* UTF-8 */
}

static int state_eq(const cp_state *a, const cp_state *b)
{
    return a->pid == b->pid && a->seq == b->seq && a->t_ms == b->t_ms && a->pos[0] == b->pos[0] && a->pos[1] == b->pos[1] && a->pos[2] == b->pos[2] &&
           fabsf(a->yaw - b->yaw) < 1.0f / 10000.0f && a->action == b->action && a->hp == b->hp && a->hpmax == b->hpmax && fabsf(a->aim - b->aim) < 0.006f &&
           fabsf(a->aimv[0] - b->aimv[0]) < 0.0001f && fabsf(a->aimv[1] - b->aimv[1]) < 0.0001f && fabsf(a->aimv[2] - b->aimv[2]) < 0.0001f && a->weapon == b->weapon &&
           a->epoch == b->epoch && a->flags == b->flags && a->pstate == b->pstate && a->ctrl == b->ctrl && a->item[0] == b->item[0] && a->item[1] == b->item[1] && a->item[2] == b->item[2] &&
           a->move == b->move && a->elev == b->elev;
}

static void make_all(cp_msg *m, int *n)
{
    int k = 0;
    memset(m, 0, 24 * sizeof *m);
    m[k].type = CP_HELLO;
    m[k].u.hello.version = 1, m[k].u.hello.min_version = 1, m[k].u.hello.want_char = CP_CHAR_MOIRA, m[k].u.hello.flags = CP_PF_BOT;
    for (int i = 0; i < 8; i++) m[k].u.hello.exe_tag[i] = (uint8_t)(0x55 + i);
    snprintf(m[k].u.hello.name, sizeof m[k].u.hello.name, "Moira Player");
    snprintf(m[k].u.hello.mod, sizeof m[k].u.hello.mod, "0.4.0-test");
    k++;
    m[k].type = CP_WELCOME;
    m[k].u.welcome.version = 1, m[k].u.welcome.pid = 1, m[k].u.welcome.flags = 0, m[k].u.welcome.host_ms = 123456, m[k].u.welcome.phase = CP_PHASE_LOBBY, m[k].u.welcome.epoch = 0;
    sample_session(&m[k].u.welcome.session), sample_lobby(&m[k].u.welcome.lobby);
    snprintf(m[k].u.welcome.mod, sizeof m[k].u.welcome.mod, "0.2.0");
    k++;
    m[k].type = CP_REFUSE, m[k].u.refuse.code = CP_REF_VERSION, snprintf(m[k].u.refuse.text, sizeof m[k].u.refuse.text, "different co-op protocol version");
    snprintf(m[k].u.refuse.mod, sizeof m[k].u.refuse.mod, "0.2.0-rc.x");
    k++;
    m[k].type = CP_NOTICE, m[k].u.refuse.code = CP_NOTE_SLOT_TAKEN, snprintf(m[k].u.refuse.text, sizeof m[k].u.refuse.text, "claire is taken");
    k++;
    m[k].type = CP_SESSION, sample_session(&m[k].u.session);
    k++;
    m[k].type = CP_LOBBY_PICK, m[k].u.pick.character = CP_CHAR_NATALIA;
    k++;
    m[k].type = CP_LOBBY_READY, m[k].u.ready.ready = 1;
    k++;
    m[k].type = CP_LOBBY_STATE, sample_lobby(&m[k].u.lobby);
    k++;
    m[k].type = CP_START, sample_session(&m[k].u.start.session), sample_lobby(&m[k].u.start.lobby);
    k++;
    m[k].type = CP_LOAD_BEGIN, m[k].u.load.pid = 1, m[k].u.load.epoch = 4, m[k].u.load.area_id = 0x1234;
    k++;
    m[k].type = CP_LOAD_DONE, m[k].u.load.pid = 1, m[k].u.load.epoch = 4;
    k++;
    m[k].type = CP_GO, m[k].u.load.epoch = 4;
    k++;
    m[k].type = CP_EVENT, m[k].u.event.pid = 1, m[k].u.event.kind = CP_EV_PICKUP, m[k].u.event.eseq = 9, m[k].u.event.t_ms = 5000, m[k].u.event.epoch = 4, m[k].u.event.len = 6;
    memcpy(m[k].u.event.body, "\x01\x02\x03\x04\x05\x06", 6);
    k++;
    m[k].type = CP_WORLD, m[k].u.world.pid = 1, m[k].u.world.kind = 2, m[k].u.world.seq = 65534, m[k].u.world.t_ms = 777000, m[k].u.world.epoch = 3, m[k].u.world.len = 250;
    for (int i = 0; i < 250; i++) m[k].u.world.body[i] = (uint8_t)(i * 13 + 1);
    k++;
    m[k].type = CP_HEARTBEAT, m[k].u.hb.pid = 1, m[k].u.hb.t_ms = 1000, m[k].u.hb.echo_t_ms = 900, m[k].u.hb.echo_dwell_ms = 12, m[k].u.hb.phase = CP_PHASE_PLAYING, m[k].u.hb.epoch = 4, m[k].u.hb.state_seq = 65535;
    k++;
    m[k].type = CP_LEAVE, m[k].u.leave.reason = 2;
    k++;
    cp_state *s = &m[k].u.state;
    m[k].type = CP_STATE;
    s->pid = 1, s->seq = 513, s->t_ms = 0xDEADBEEFu, s->pos[0] = 123.5f, s->pos[1] = -4.25f, s->pos[2] = 99999.0f, s->yaw = -1.5707964f, s->action = 3, s->hp = 640, s->hpmax = 800, s->aim = -12.34f;
    s->aimv[0] = 0.5f, s->aimv[1] = -0.25f, s->aimv[2] = 0.8f, s->weapon = 5, s->epoch = 4, s->flags = CP_SF_AIMING | CP_SF_FLASHLIGHT;
    s->pstate = 0x80000010u, s->ctrl = 0x04000004u, s->item[0] = 0x14d, s->item[1] = 0, s->item[2] = 0x201, s->move = 200, s->elev = -1234;
    k++;
    cp_pad *pd = &m[k].u.pad;
    m[k].type = CP_PAD;
    pd->pid = 1, pd->epoch = 4, pd->flags = CP_SF_PAUSED | CP_SF_CUTSCENE, pd->seq = 65535, pd->t_ms = 0xFFFFFF00u, pd->count = 4;
    pd->rec[0].buttons = 0x1010, pd->rec[0].lt = 255, pd->rec[0].rt = 7, pd->rec[0].lx = -32768, pd->rec[0].ly = 32767, pd->rec[0].rx = 100, pd->rec[0].ry = -100;
    pd->rec[1].buttons = 0x1010, pd->rec[1].lt = 200, pd->rec[1].lx = -30000, pd->rec[1].ly = 32767, pd->rec[1].rx = 100, pd->rec[1].ry = -100, pd->dt[1] = 16;
    pd->rec[2] = pd->rec[1], pd->rec[2].buttons = 0, pd->dt[2] = 255;
    pd->rec[3].rt = 1, pd->dt[3] = 17;
    k++;
    *n = k;
}

static int msg_eq(const cp_msg *a, const cp_msg *b)
{
    if (a->type != b->type) return 0;
    switch (a->type) {
    case CP_STATE: return state_eq(&a->u.state, &b->u.state);
    case CP_PAD: {
        const cp_pad *x = &a->u.pad, *y = &b->u.pad;
        if (x->pid != y->pid || x->epoch != y->epoch || x->flags != y->flags || x->seq != y->seq || x->t_ms != y->t_ms || x->count != y->count) return 0;
        for (unsigned i = 0; i < x->count; i++)
            if (memcmp(&x->rec[i], &y->rec[i], sizeof x->rec[i]) || (i && x->dt[i] != y->dt[i])) return 0;
        return 1;
    }
    default: {
        /* unions with padding: compare re-encodings instead of memcmp */
        uint8_t x[CP_MAX_PACKET], y[CP_MAX_PACKET];
        size_t nx = cp_encode(a, x, sizeof x), ny = cp_encode(b, y, sizeof y);
        return nx && nx == ny && !memcmp(x, y, nx);
    }
    }
}

int main(void)
{
    cp_msg all[24], dec;
    int n = 0;
    make_all(all, &n);

    /* 1. round trip, strict prefixes fail, trailing bytes are ignored */
    for (int i = 0; i < n; i++) {
        uint8_t buf[CP_MAX_PACKET + 16];
        size_t len = cp_encode(&all[i], buf, CP_MAX_PACKET);
        CHECK(len > 0);
        CHECK(cp_decode(buf, len, &dec));
        CHECK(msg_eq(&all[i], &dec));
        CHECK(len <= CP_MAX_PACKET);
        /* WELCOME / REFUSE end with an optional tail (the host's mod version, 0.2.0): cut before or inside it, the message is the older form and decodes with mod "" */
        size_t tail = all[i].type == CP_WELCOME ? 1 + strlen(all[i].u.welcome.mod) : all[i].type == CP_REFUSE ? 1 + strlen(all[i].u.refuse.mod) : 0;
        for (size_t cut = 0; cut < len; cut++) {
            if (cut >= len - tail) {
                CHECK(cp_decode(buf, cut, &dec) && (all[i].type == CP_WELCOME ? dec.u.welcome.mod : dec.u.refuse.mod)[0] == 0);
                continue;
            }
            CHECK(!cp_decode(buf, cut, &dec));
        }
        if (tail) CHECK(!strcmp(all[i].type == CP_WELCOME ? dec.u.welcome.mod : dec.u.refuse.mod, "") && cp_decode(buf, len, &dec) &&
                        !strcmp(all[i].type == CP_WELCOME ? dec.u.welcome.mod : dec.u.refuse.mod, all[i].type == CP_WELCOME ? all[i].u.welcome.mod : all[i].u.refuse.mod));
        memset(buf + len, 0xAB, 5);
        CHECK(cp_decode(buf, len + 5, &dec));
        CHECK(msg_eq(&all[i], &dec));
        CHECK(cp_encode(&all[i], buf, len - 1) == 0);   /* does not fit */
    }

    /* 2. byte vectors (docs/protocol.md): a STATE and a HELLO */
    {
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_STATE;
        m.u.state.pid = 1, m.u.state.seq = 0x0102, m.u.state.t_ms = 0x00001000, m.u.state.pos[0] = 1.0f, m.u.state.pos[1] = 2.0f, m.u.state.pos[2] = -1.0f, m.u.state.yaw = 1.0f;
        m.u.state.action = 3, m.u.state.hp = 100, m.u.state.hpmax = 200, m.u.state.aim = 45.0f, m.u.state.aimv[2] = 1.0f, m.u.state.weapon = 7, m.u.state.epoch = 2, m.u.state.flags = 0x0101;
        m.u.state.pstate = 0x11223344u, m.u.state.ctrl = 0x04, m.u.state.item[0] = 0x014d, m.u.state.item[2] = 0x0201, m.u.state.move = 255, m.u.state.elev = -500;
        uint8_t b[96];
        char h[200];
        size_t len = cp_encode(&m, b, sizeof b);
        hex(b, len, h);
        CHECK(len == 60);
        static const char *const want_state = "50" "01" "0201" "00100000" "0000803f" "00000040" "000080bf" "1027" "0300" "6400" "c800" "9411" "000000000040" "07" "02" "0101" "44332211" "04000000" "4d01" "0000" "0102" "ff00" "0cfe" "0000";
        CHECK(!strcmp(h, want_state));
        if (strcmp(h, want_state)) printf("state vector: %s\n", h);
        memset(&m, 0, sizeof m);
        m.type = CP_HELLO;
        m.u.hello.version = 2, m.u.hello.min_version = 2, m.u.hello.want_char = 1, m.u.hello.flags = 0;
        snprintf(m.u.hello.name, sizeof m.u.hello.name, "Bob");
        snprintf(m.u.hello.mod, sizeof m.u.hello.mod, "0.1");
        len = cp_encode(&m, b, sizeof b);
        hex(b, len, h);
        static const char *const want_hello = "01" "52314350" "0200" "0200" "0000000000000000" "01" "00" "03426f62" "03302e31";
        CHECK(!strcmp(h, want_hello));
        if (strcmp(h, want_hello)) printf("hello vector: %s\n", h);
    }

    /* 2b. CP_PAD (version 3): number, version gate constants, byte vector, delta coding sizes, malformed */
    {
        CHECK(CP_PAD == 0x52 && CP_WORLD == 0x51 && CP_STATE == 0x50);
        CHECK(CP_PROTO_VERSION == 3 && CP_PROTO_MIN == 3);   /* version 2 peers do not know CP_PAD: refused at HELLO (tests/test_session.c) */
        CHECK(!strcmp(cp_type_name(CP_PAD), "PAD"));
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_PAD;
        cp_pad *pd = &m.u.pad;
        pd->pid = 1, pd->epoch = 2, pd->flags = 0x0801, pd->seq = 0x0102, pd->t_ms = 0x00001000, pd->count = 2;
        pd->rec[0].buttons = 0x1000, pd->rec[0].rt = 255, pd->rec[0].lx = 1000;
        pd->rec[1].buttons = 0x1000, pd->rec[1].lx = 900, pd->dt[1] = 16;   /* rt 255 -> 0 and lx 1000 -> 900 against the newer record */
        uint8_t b[96];
        char h[200];
        size_t len = cp_encode(&m, b, sizeof b);
        hex(b, len, h);
        /* 52 pid epoch flags seq t_ms count | mask 0b0001101 (buttons, rt, lx) buttons rt lx | dt mask 0b0000100.. */
        static const char *const want_pad = "52" "01" "02" "0108" "0201" "00100000" "02" "0d" "0010" "ff" "e803" "10" "0c" "00" "8403";
        CHECK(!strcmp(h, want_pad));
        if (strcmp(h, want_pad)) printf("pad vector: %s\n", h);
        CHECK(cp_decode(b, len, &dec) && msg_eq(&m, &dec) && dec.u.pad.rec[1].rt == 0 && dec.u.pad.rec[1].lx == 900);
        /* sizes: an idle pad is tiny, a worst case four-record packet stays far below CP_MAX_PACKET */
        memset(&m, 0, sizeof m);
        m.type = CP_PAD, m.u.pad.count = 1;
        CHECK(cp_encode(&m, b, sizeof b) == 12 + 1);
        pd = &m.u.pad;
        pd->count = 4;
        for (int i = 0; i < 4; i++) {
            pd->rec[i].buttons = (uint16_t)(0x1111 * (i + 1)), pd->rec[i].lt = (uint8_t)(10 + i), pd->rec[i].rt = (uint8_t)(20 + i);
            pd->rec[i].lx = (int16_t)(1000 * (i + 1)), pd->rec[i].ly = (int16_t)(-1000 * (i + 1)), pd->rec[i].rx = (int16_t)(5 * i + 1), pd->rec[i].ry = (int16_t)(-5 * i - 1), pd->dt[i] = (uint8_t)(i ? 16 : 0);
        }
        len = cp_encode(&m, b, sizeof b);
        CHECK(len == 12 + 4 * (1 + 12) + 3);   /* header 12, per record mask + 12 field bytes, three dt bytes */
        CHECK(len <= 80);
        /* count 0 / 5, reserved mask bit, truncation */
        pd->count = 0;
        CHECK(cp_encode(&m, b, sizeof b) == 0);
        pd->count = 5;
        CHECK(cp_encode(&m, b, sizeof b) == 0);
        pd->count = 2;
        len = cp_encode(&m, b, sizeof b);
        CHECK(len > 0 && cp_decode(b, len, &dec));
        uint8_t c[96];
        memcpy(c, b, len);
        c[12] = 0;   /* count byte at offset 11: 0 */
        c[11] = 0;
        CHECK(!cp_decode(c, len, &dec));
        memcpy(c, b, len);
        c[11] = 5;
        CHECK(!cp_decode(c, len, &dec));
        memcpy(c, b, len);
        c[12] |= 0x80;   /* mask of the first record: reserved bit */
        CHECK(!cp_decode(c, len, &dec));
    }

    /* 3. quantisation: yaw wraps, NaN becomes 0, huge values clamp */
    {
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_STATE;
        m.u.state.yaw = 7.0f;   /* 7 rad = 0.7168 rad */
        m.u.state.aim = 1e9f;
        m.u.state.pos[0] = NAN;
        m.u.state.aimv[0] = 9.0f;
        m.u.state.action = 0x80000000u;
        uint8_t b[64];
        size_t len = cp_encode(&m, b, sizeof b);
        CHECK(len == 60 && cp_decode(b, len, &dec));
        CHECK(fabsf(dec.u.state.yaw - (7.0f - 6.2831853f)) < 0.001f);
        CHECK(dec.u.state.aim == 327.67f);
        CHECK(dec.u.state.pos[0] == 0.0f);
        CHECK(dec.u.state.aimv[0] > 1.99f && dec.u.state.aimv[0] < 2.0f);
        CHECK(dec.u.state.action == 0x80000000u);
        m.u.state.action = 0xFFFF;
        len = cp_encode(&m, b, sizeof b);
        CHECK(cp_decode(b, len, &dec) && dec.u.state.action == 0xFFFE);
    }

    /* 4. malformed input: bad magic, bad counts, oversize event, unknown type, control characters in names */
    {
        uint8_t b[CP_MAX_PACKET];
        size_t len = cp_encode(&all[0], b, sizeof b);
        b[1] ^= 0xFF;
        CHECK(!cp_decode(b, len, &dec));
        len = cp_encode(&all[7], b, sizeof b);   /* LOBBY_STATE: count byte at 1 + 4 */
        b[5] = 9;
        CHECK(!cp_decode(b, len, &dec));
        uint8_t ev[40];
        size_t el = cp_encode(&all[12], ev, sizeof ev);
        ev[el - 7] = 200;   /* length byte > CP_EVENT_MAX (body is 6 bytes, length byte right before) */
        CHECK(!cp_decode(ev, el, &dec));
        uint8_t unk[4] = {0x77, 1, 2, 3};
        CHECK(!cp_decode(unk, 4, &dec));
        CHECK(!cp_decode(unk, 0, &dec));
        CHECK(!cp_decode(NULL, 5, &dec));
        len = cp_encode(&all[0], b, sizeof b);
        for (size_t i = 0; i + 4 < len; i++)
            if (!memcmp(b + i, "Moira Player", 12)) b[i] = 0x07;   /* BEL in a name */
        CHECK(cp_decode(b, len, &dec) && dec.u.hello.name[0] == '?');
    }

    /* 5. fuzz: random packets and random mutations of valid packets never crash, and what decodes re-encodes */
    {
        unsigned seed = 1;
        int decoded = 0;
        for (int it = 0; it < 200000; it++) {
            uint8_t b[300];
            seed = seed * 1664525u + 1013904223u;
            size_t len = (seed >> 8) % 300;
            if ((seed & 7) < 3) {
                for (size_t i = 0; i < len; i++) seed = seed * 1664525u + 1013904223u, b[i] = (uint8_t)(seed >> 16);
                if (len) b[0] = (uint8_t)(1 + (seed >> 3) % 0x60);
            } else {
                cp_msg *src = &all[(seed >> 4) % (unsigned)n];
                len = cp_encode(src, b, sizeof b);
                for (int k = 0; k < 3; k++) {
                    seed = seed * 1664525u + 1013904223u;
                    b[(seed >> 8) % len] = (uint8_t)(seed >> 20);
                }
            }
            if (cp_decode(b, len, &dec)) {
                decoded++;
                uint8_t o[CP_MAX_PACKET];
                CHECK(cp_encode(&dec, o, sizeof o) > 0);   /* every decoded message can be encoded again */
            }
        }
        printf("fuzz: %d of 200000 packets decoded\n", decoded);
    }

    /* 6. sequence arithmetic */
    CHECK(cp_seq16_newer(5, 4) && !cp_seq16_newer(4, 5) && !cp_seq16_newer(5, 5));
    CHECK(cp_seq16_newer(0, 65535) && !cp_seq16_newer(65535, 0) && cp_seq16_newer(100, 65500));
    CHECK(cp_seq8_newer(0, 255) && cp_seq8_newer(2, 1) && !cp_seq8_newer(1, 2));
    CHECK(cp_char_parse("Moira") == CP_CHAR_MOIRA && cp_char_parse("3") == 3 && cp_char_parse("x") == CP_CHAR_NONE && cp_char_parse("claire ") == CP_CHAR_NONE);

    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
