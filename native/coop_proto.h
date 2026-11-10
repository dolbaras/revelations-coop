/*
 * Wire protocol of the Revelations 2 co-op mod (see docs/protocol.md). Pure C, no Windows or game dependencies: the same
 * file is compiled into the dinput8 proxy, the headless bot and the unit tests.
 *
 * All integers are little endian, floats IEEE-754 binary32, strings are u8 length + bytes (UTF-8, at most 255).
 * Every packet starts with a one byte message type. A packet is one transport message (no framing of our own).
 * Forward rule: a decoder takes the fields it knows and ignores trailing bytes, so a newer sender may append fields to an
 * existing message without a version bump; changing or removing a field needs CP_PROTO_VERSION + 1.
 */
#ifndef COOP_PROTO_H
#define COOP_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define CP_PROTO_VERSION 3u   /* layout of the messages below (2: STATE grew from 40 to 60 bytes, stage 5; 3: CP_PAD, the partner's pad record for the Revelations 1 puppet) */
#define CP_PROTO_MIN 3u       /* oldest version this build still talks: a version 2 peer does not know CP_PAD (it would drop the pad records silently and the puppet would stand), so it is refused at HELLO by the version gate */
#define CP_MAGIC 0x50433152u  /* bytes 'R' '1' 'C' 'P' */
#define CP_MAX_PLAYERS 4
#define CP_NAME_MAX 24
#define CP_TEXT_MAX 120
#define CP_MOD_MAX 15
#define CP_EVENT_MAX 96
#define CP_PAD_MAX_REC 4       /* records one CP_PAD carries (the newest and up to three older ones: loss cover on the unreliable channel) */
#define CP_WORLD_MAX 400       /* body of a WORLD message (stage 6: enemy / object records, unreliable) */
#define CP_MAX_PACKET 512     /* every message of this protocol fits (state 60, lobby ~ 4 * 32) */

/* transport channels: 0 = reliable + ordered, 1 = unreliable + sequenced (newest wins) */
enum { CP_CH_CONTROL = 0, CP_CH_STATE = 1, CP_CHANNELS = 2 };

enum cp_type {
    CP_HELLO = 0x01,        /* client -> host, first message after connect */
    CP_WELCOME = 0x02,      /* host -> client: accepted, your player id, session and lobby */
    CP_REFUSE = 0x03,       /* host -> client: not accepted (code + text), the host closes the connection afterwards */
    CP_SESSION = 0x04,      /* host -> all: session settings changed */
    CP_LOBBY_PICK = 0x05,   /* client -> host: I want this character */
    CP_LOBBY_READY = 0x06,  /* client -> host: ready flag */
    CP_LOBBY_STATE = 0x07,  /* host -> all: the whole lobby table (revision counted) */
    CP_START = 0x08,        /* host -> all: begin the shared game (session + final lobby) */
    CP_NOTICE = 0x09,       /* host -> one client: a request was not honoured (code + text) */
    CP_LOAD_BEGIN = 0x10,   /* any -> all (via host): I begin loading level instance <epoch> of area <id> */
    CP_LOAD_DONE = 0x11,    /* any -> host: loaded and ready */
    CP_GO = 0x12,           /* host -> all: everybody finished loading <epoch>, run */
    CP_EVENT = 0x20,        /* any -> all (via host): reliable game event (fire, hit, death, pickup, door ...) */
    CP_HEARTBEAT = 0x30,    /* any -> any, unreliable: liveness, RTT and clock offset */
    CP_LEAVE = 0x3F,        /* any -> any: I am leaving (reason) */
    CP_STATE = 0x50,        /* any -> all (via host), unreliable, 20 Hz: the state of my character */
    CP_WORLD = 0x51,        /* any -> all (via host), unreliable, sequenced: batches of world records (enemies, objects), stage 6 */
    CP_PAD = 0x52           /* any -> all (via host), unreliable, sequenced: the sender's pad record(s), up to 60 Hz, drives the puppet of the sender on the other PC (Revelations 1, version 3) */
};

/* Revelations 1 has no fixed cast: a session carries ROLES, not names. The LEAD is the unit slot 0 human (the one a single player controls), the
 * PARTNER is the unit slot 1 human (normally the AI companion); which person that is comes from the section's pair (the story decides). The values
 * and the pair logic (2 / 3 = the second pair, session.part) are the Revelations 2 ones; the old names stay as aliases for the shared session code. */
enum cp_character { CP_CHAR_LEAD = 0, CP_CHAR_PARTNER = 1, CP_CHAR_LEAD_B = 2, CP_CHAR_PARTNER_B = 3, CP_CHAR_NONE = 255 };
#define CP_CHAR_CLAIRE CP_CHAR_LEAD
#define CP_CHAR_MOIRA CP_CHAR_PARTNER
#define CP_CHAR_BARRY CP_CHAR_LEAD_B
#define CP_CHAR_NATALIA CP_CHAR_PARTNER_B
#define CP_CHAR_SLOT(c) ((c) & 1)   /* the player slot the role occupies: lead = 0, partner = 1 */

enum cp_refuse_code {
    CP_REF_VERSION = 1,     /* protocol versions do not overlap */
    CP_REF_BUILD = 2,       /* another game build / exe */
    CP_REF_FULL = 3,
    CP_REF_STARTED = 4,     /* the game already started and this is not a known player */
    CP_REF_BAD = 5,         /* malformed or unexpected first message */
    CP_REF_MODVER = 6,      /* another release version of the co-op mod (0.2.0 on: the host compares HELLO.mod with its own, bots excepted) */
    CP_REF_REMOVED = 7      /* Rev1 lobby-must: the host removed this player (also after WELCOME: the client ends its session with LOST a=3) */
};
enum cp_notice_code { CP_NOTE_SLOT_TAKEN = 1, CP_NOTE_NOT_READY = 2, CP_NOTE_REJECTED = 3, CP_NOTE_INFO = 4 };

enum cp_player_flags { CP_PF_HOST = 1, CP_PF_CONNECTED = 2, CP_PF_INGAME = 4, CP_PF_LOADED = 8, CP_PF_BOT = 16 };

/* PLAYER_STATE flags */
enum cp_state_flags {
    CP_SF_AIMING = 1, CP_SF_FIRING = 2, CP_SF_RELOADING = 4, CP_SF_CROUCH = 8, CP_SF_MELEE = 16, CP_SF_FLASHLIGHT = 32, CP_SF_DEAD = 64,
    CP_SF_DOWN = 128, CP_SF_LOADING = 256, CP_SF_CUTSCENE = 512, CP_SF_TELEPORT = 1024 /* do not interpolate to this sample */, CP_SF_PAUSED = 2048 /* the sender's pause menu is open */,
    CP_SF_HELD = 4096 /* the sender's human is held by an enemy (action INT_MIN, the hold camera, no scene running): not a cutscene, the body keeps being applied (stage 6e) */
};

/* the flags a CP_PAD carries (the sender's hold conditions, see puppet1.h); the rest of cp_state_flags is STATE only. Revelations 1 reads cp_state like this: pos = unit +0x40..,
 * yaw = +0x1180, action = upper-body state id (low byte) | mode +0xF08 (second byte), hp / hpmax = +0xE38 / +0xE40, pstate = +0xF3C, ctrl = +0xF44, weapon = bag slot +0xD4;
 * aim, aimv, elev stay 0 (the pad record carries the aim). The size stays 60 bytes. */
#define CP_PAD_FLAGS_MASK (CP_SF_DEAD | CP_SF_DOWN | CP_SF_LOADING | CP_SF_CUTSCENE | CP_SF_PAUSED | CP_SF_HELD)

/* event kinds (CP_EVENT); only the envelope is implemented, bodies are documented in docs/protocol.md */
enum cp_event_kind {
    CP_EV_NONE = 0, CP_EV_FIRE = 1, CP_EV_HIT = 2, CP_EV_DEATH = 3, CP_EV_PICKUP = 4, CP_EV_DOOR = 5, CP_EV_INTERACT = 6, CP_EV_ITEMUSE = 7,
    CP_EV_CHECKPOINT = 8, CP_EV_CUTSCENE = 9, CP_EV_STAT = 10, CP_EV_TEXT = 11, CP_EV_USER = 0x100
};

/* Revelations 1 world sync (world1.c, notes/net-world-impl.md). It rides on the two existing envelopes, so it adds NO message type and needs NO protocol version bump:
 *   reliable lane  CP_EVENT  kind CP_EV_W1: body = epoch u8, seq u16, sub u8, payload (the model retransmits until acked, so it is also safe on a lossy line)
 *   unreliable     CP_WORLD  kinds CP_WK_W1_ENEMIES / CP_WK_W1_ROSTER / CP_WK_W1_ACK: body starts with the epoch u8
 * The numbers are free in this tree: CP_EV_USER + 0x40 is not used by the session code; CP_WORLD kinds 0x21..0x23 are not used by Revelations 1 (Revelations 2 keeps 1).
 * 0x51 = CP_WORLD and 0x52 = CP_PAD (puppet) are top-level types and are NOT touched. A peer that does not know these kinds ignores them (the session relays unknown kinds). */
#define CP_EV_W1 (CP_EV_USER + 0x40)
#define CP_EV_SFPRE (CP_EV_USER + 0x43)   /* ep1-probe: the host's pre-load flag digest at its restart (sf1_pre_*; 0x141 barrier, 0x142 downed) */
enum cp_w1_world_kind { CP_WK_W1_ENEMIES = 0x21, CP_WK_W1_ROSTER = 0x22, CP_WK_W1_ACK = 0x23 };
/* sub types of CP_EV_W1 */
enum cp_w1_sub { CP_W1_SPAWN = 1, CP_W1_HIT = 2, CP_W1_DEATH = 3, CP_W1_DESPAWN = 4, CP_W1_PICKUP = 5, CP_W1_DOOR = 6, CP_W1_FLAG = 7, CP_W1_REQ = 8,
                 /* 9 is kept for LOOT */ CP_W1_PCHIT = 10 /* host -> client: an enemy's blow on the client's human (the host does not apply it to the puppet) */,
                 /* the story mirror (sf1.c, ep1-world): handed to ops.ext by the model, encoded by sf1 */
                 CP_W1_SFPROP = 11 /* guest -> host: a proposal (a flag set by the guest's own engine) */, CP_W1_SFDIG = 12 /* host -> guest: flag digest after the quiet window */,
                 CP_W1_SFRATE = 13 /* the Genesis analysis rate: absolute from the host / delta from the guest */, CP_W1_PIECE = 14 /* both: a set piece broke (key4, class) */,
                 CP_W1_PROMPT = 15 /* both: a story prompt pressed by this PC's human (script hash, command; prompt1.h) */ };
#define CP_W1_EXT_FIRST 11   /* sub types 11..31 are delivered to ops.ext (an older model acknowledges and ignores them) */
#define CP_W1_EXT_LAST 31
#define CP_W1_ZONE 16   /* round 9, both: this PC's own human in / out of a story zone (zone1.h: ECD path key, record); the other PC counts its puppet there only then */
#define CP_W1_LOCAL 20  /* rev1 live 11, client -> host: an enemy the host does not simulate died on the client's own AI / was handed back with its hp (local1.h) */
#define CP_W1_STAGE 21  /* rev1 live 12, both: the stage part codes of the floor under this PC's active human (stage1.h); the watching PC streams them */
#define CP_W1_RLOC 22   /* rev1 live 13, client -> host: positions / hp of the enemies run on the client's AI (watch1.h): the host shows them as replicas */
#define CP_W1_HITX 23   /* rev1 live 14, client -> host: the own human's blow on a replica with its real HitInfo (hitx1.h): the host plays it through the Raid applier */
#define CP_W1_CAM 24    /* rev1 live 14, both: the active PC's gameplay camera while the other human is switched off (watchcam1.h): the watching PC shows it */
#define CP_W1_MOT 25    /* rev1 live 14, both: a motion layer of an enemy run on this PC's own AI (mot1.h): the other PC's replica plays it */
#define CP_W1_BAG 26    /* rev1 live 14, both: this PC's human's three guns and the current one (bag1.h): the other PC's puppet carries / holds them */

enum cp_phase { CP_PHASE_NONE = 0, CP_PHASE_LOBBY = 1, CP_PHASE_LOADING = 2, CP_PHASE_PLAYING = 3 };

typedef struct {
    uint32_t id;            /* session id chosen by the host */
    uint8_t game_type;      /* 0 story campaign (more later) */
    uint8_t episode;        /* 1..4 */
    uint8_t part;           /* 0 Claire/Moira half, 1 Barry/Natalia half */
    uint8_t difficulty;
    uint8_t max_players;    /* 2 for now */
    uint8_t flags;
    uint32_t area_id;       /* game area the host starts in (informational) */
    uint32_t seed;
} cp_session;

typedef struct {
    uint8_t pid;
    uint8_t character;      /* enum cp_character or CP_CHAR_NONE */
    uint8_t ready;
    uint8_t flags;          /* enum cp_player_flags */
    char name[CP_NAME_MAX + 1];
} cp_player;

typedef struct {
    uint32_t rev;           /* incremented by the host at every change */
    uint8_t count;
    cp_player p[CP_MAX_PLAYERS];
} cp_lobby;

typedef struct {
    uint16_t version, min_version;
    uint8_t exe_tag[8];     /* first 8 bytes of the game exe's SHA-256, zeros = unknown (bot) */
    uint8_t want_char;
    uint8_t flags;          /* CP_PF_BOT */
    char name[CP_NAME_MAX + 1];
    char mod[CP_MOD_MAX + 1];
} cp_hello;

typedef struct {
    uint8_t pid;
    uint16_t seq;           /* per sender, wraps */
    uint32_t t_ms;          /* sender's monotonic milliseconds */
    float pos[3];
    float yaw;              /* radians, -pi..pi */
    uint32_t action;        /* the game's action id (player +0x1A54); 0x80000000 = waiting is sent as 0xFFFF */
    int16_t hp, hpmax;
    float aim;              /* degrees (player +0x7950) */
    float aimv[3];          /* aim vector (player +0x7960), roughly unit length */
    uint8_t weapon;         /* which weapon slot is in the hand: 0 none, 1..3 weapon slots, 4 special (FUN_006fb060) */
    uint8_t epoch;          /* level instance counter, see the load barrier */
    uint16_t flags;         /* enum cp_state_flags */
    /* version 2 */
    uint32_t pstate;        /* the game's own player state flags (+0x37B8): replayed on the puppet */
    uint32_t ctrl;          /* the game's player control state flags (+0x78E8: bit 2 = aiming, ...) */
    uint16_t item[3];       /* item ids in the weapon slots 1..3 (0 = empty): inventory entry +4 of the entry FUN_006fb270(slot) names */
    uint8_t move;           /* locomotion intensity 0..255 (+0x792C: 0 idle, 255 full walk/run blend) */
    int16_t elev;           /* aim elevation, degrees * 100 (+0x7954) */
} cp_state;

typedef struct {
    uint8_t pid;
    uint16_t kind;          /* enum cp_event_kind or CP_EV_USER + n */
    uint16_t eseq;          /* per sender event counter */
    uint32_t t_ms;
    uint8_t epoch;
    uint8_t len;
    uint8_t body[CP_EVENT_MAX];
} cp_event;

/* One pad record == XINPUT_GAMEPAD (coop_input.h r1_input_t, 12 bytes): what the game's own pad code reads. Own type so that this file stays free of game headers. */
typedef struct {
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} cp_pad_rec;

/* PAD: the newest record (rec[0], sequence number seq, sender time t_ms) and up to CP_PAD_MAX_REC-1 older changed records (rec[i] has the sequence number seq - i
 * and the time t_ms - dt[1] - ... - dt[i], each dt capped at 255 ms). A record is only numbered when the pad CHANGED (the sender deduplicates), so a packet that repeats
 * the newest record as a keepalive carries the same seq as the previous one. The receiver applies every record it has not applied yet, oldest first.
 * Wire: 52 pid epoch flags(u16) seq(u16) t_ms(u32) count(u8), then per record [dt(u8), not for the first] mask(u8) and the fields of the mask that differ from the
 * previous record of the packet (the first one differs from the all-zero record): bit0 buttons u16, bit1 lt u8, bit2 rt u8, bit3 lx, bit4 ly, bit5 rx, bit6 ry (i16);
 * bit 7 is reserved and must be 0. */
typedef struct {
    uint8_t pid;
    uint8_t epoch;          /* level instance, a receiver drops other epochs */
    uint16_t flags;         /* enum cp_state_flags subset of the sender NOW (PAUSED, CUTSCENE, LOADING, DEAD, DOWN, HELD): the hold rules of the puppet do not wait for the 20 Hz STATE */
    uint16_t seq;           /* sequence number of rec[0] */
    uint32_t t_ms;          /* sender clock of rec[0] */
    uint8_t count;          /* 1..CP_PAD_MAX_REC */
    uint8_t dt[CP_PAD_MAX_REC];   /* dt[0] unused (0) */
    cp_pad_rec rec[CP_PAD_MAX_REC];
} cp_pad;

typedef struct {
    uint8_t pid;
    uint8_t kind;           /* enum cp_world_kind */
    uint16_t seq;           /* per sender and kind-independent counter; a receiver keeps only newer ones */
    uint32_t t_ms;          /* sender's clock */
    uint8_t epoch;          /* level instance, a receiver drops other epochs */
    uint16_t len;
    uint8_t body[CP_WORLD_MAX];
} cp_world;

typedef struct {
    uint8_t pid;
    uint32_t t_ms;          /* sender's clock now */
    uint32_t echo_t_ms;     /* the t_ms of the last heartbeat received from the peer (0 = none yet) */
    uint32_t echo_dwell_ms; /* how long ago that one arrived */
    uint8_t phase;          /* enum cp_phase */
    uint8_t epoch;
    uint16_t state_seq;     /* last state sequence sent */
} cp_heartbeat;

typedef struct {
    uint8_t type;           /* enum cp_type */
    union {
        cp_hello hello;
        /* WELCOME / REFUSE end with an optional tail: `mod` = the host's mod version (0.2.0 on; an older host sends none: decoded as ""), flags CP_PF_BOT = the host is a bot */
        struct { uint16_t version; uint8_t pid, flags; uint32_t host_ms; uint8_t phase, epoch; cp_session session; cp_lobby lobby; char mod[CP_MOD_MAX + 1]; } welcome;
        struct { uint8_t code; char text[CP_TEXT_MAX + 1]; char mod[CP_MOD_MAX + 1]; } refuse;   /* REFUSE and NOTICE (`mod`: REFUSE only) */
        cp_session session;
        struct { uint8_t character; } pick;
        struct { uint8_t ready; } ready;
        cp_lobby lobby;
        struct { cp_session session; cp_lobby lobby; } start;
        struct { uint8_t pid, epoch; uint32_t area_id; } load;         /* LOAD_BEGIN (pid = who), LOAD_DONE / GO use epoch (+ pid) */
        cp_event event;
        cp_world world;
        cp_pad pad;
        cp_heartbeat hb;
        struct { uint8_t reason; } leave;
        cp_state state;
    } u;
} cp_msg;

/* Encode: returns the packet length, 0 when it does not fit or the type is unknown. Decode: 1 = ok, 0 = malformed (short, bad magic, bad count). */
size_t cp_encode(const cp_msg *m, uint8_t *buf, size_t cap);
int cp_decode(const uint8_t *buf, size_t len, cp_msg *m);

const char *cp_type_name(unsigned type);
const char *cp_char_name(unsigned c);
int cp_char_parse(const char *s);           /* claire|moira|barry|natalia or 0..3, CP_CHAR_NONE when unknown */
const char *cp_refuse_text(unsigned code);

/* Sequence numbers modulo 2^16 / 2^8 / 2^32 (newer-than tests that survive wrap-around). */
static inline int cp_seq16_newer(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }
static inline int cp_seq8_newer(uint8_t a, uint8_t b) { return (int8_t)(uint8_t)(a - b) > 0; }

#endif
