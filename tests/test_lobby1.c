/*
 * Offline test of the Revelations 1 co-op lobby model (native/lobby1.c, lobby1_text.c, gmdtext.c). Native Windows / any host build, no game needed.
 *   sh tests/build.sh && tests/build/test_lobby1.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/gmdtext.h"
#include "../native/lobby1.h"
#include "../native/downed_logic.h"   /* ep1-flow: the flags byte of the Downed / revive row = the rule's own reading */

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s: ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int utf8_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p >> 5) == 6 ? 1 : (*p >> 4) == 14 ? 2 : (*p >> 3) == 30 ? 3 : -1;
        if (n < 0) return 0;
        p++;
        for (int i = 0; i < n; i++, p++)
            if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

static void ev(l1_t *l, int kind) { l1_event e = {kind, 0}; l1_event_in(l, &e); }

/* the (text, language) pairs whose word is the English one in that language too: 0 eng 1 fre 2 ita 3 ger 4 spa 5 jpn 6 rus 7 pol 8 dut 9 por 10 ptB 11 chS 12 chT */
static int same_word_ok(int id, int lang)
{
    switch (id) {
    case L1T_PARTNER: return lang == 7 || lang == 8;                         /* "partner" (Polish, Dutch) */
    case L1T_PADS: return lang == 3 || lang == 8;                            /* "Gamepads" (German, Dutch) */
    case L1T_PAD1: case L1T_PAD2: return lang == 3 || lang == 7 || lang == 8;   /* "Gamepad 1" */
    case L1T_LEG_OK: return lang != 5 && lang != 11 && lang != 12;          /* "OK": the game's own key legend says OK in these languages (Russian title menu: "(A) OK (B) Назад") */
    case L1T_SECS: return lang != 5 && lang != 6 && lang != 11 && lang != 12;   /* "%s s" */
    case L1T_EPISODE1: return lang == 3 || lang == 5 || lang == 8;           /* the game's own campaign_<lang>.gmd episode_01 is "Episode 1" there */
    default: return 0;
    }
}

/* the game's own choice of its text language (build 13298487): 0x9BC7B5 GetUserDefaultLangID -> sMain.mLanguage by the primary language (jump table 0x9BC99C), then 0x844010:
   the 13 languages it ships, English for the others. Written from the disassembly, independent of lobby1_text.c */
static int game_mlanguage(unsigned id)
{
    static const signed char by_primary[0x1E] = {-1, -1, -1, -1, -2, 17, 15, 4, 20, 1, 3, 13, 2, 20, 18, 20, 5, 0, 6, 12, 16, 11, -3, 20, 20, 10, 20, 19, 20, 14};
    unsigned p = id & 0x3FF;
    if (p < 4 || p > 0x1D) return 20;
    int m = by_primary[p];
    if (m == -2) return id == 0x0404 ? 7 : 8;     /* Chinese: Taiwan chT, else chS */
    if (m == -3) return id == 0x0416 ? 21 : 9;    /* Portuguese: Brazil ptB, else por */
    return m;
}
static int game_text_lang(unsigned m)
{
    static const char *const sfx[22] = {"jpn", "eng", "fre", "spa", "ger", "ita", "kor", "chT", "chS", "por", "rus", "pol", "dut", "fin", "swe", "dan", "nor", "cze", "hun", "slo", "oth", "ptB"};
    int l = m < 22 ? l1_lang_from_code(sfx[m]) : -1;   /* (kor / fin ... oth: not shipped, the game reads English) */
    return l < 0 ? 0 : l;
}

static void test_texts(void)
{
    for (int id = 0; id < L1T_COUNT; id++)
        for (int lang = 0; lang < L1_LANGS; lang++) {
            const char *s = l1_text(id, lang);
            CHECKF(s && s[0], "text %d lang %d empty", id, lang);
            CHECKF(utf8_ok(s), "text %d lang %d is not valid UTF-8", id, lang);
            CHECKF(strlen(s) < 280, "text %d lang %d too long (%zu)", id, lang, strlen(s));
            if (lang && !same_word_ok(id, lang))   /* lang (owner 2026-11-08): no text of a non-English language may be the English one, except the listed same words */
                CHECKF(strcmp(s, l1_text(id, 0)) != 0, "text %d lang %s equals English", id, l1_lang_codes[lang]);
        }
    /* the sentence that quotes stock words has all four placeholders in every language and expands to no placeholder */
    for (int lang = 0; lang < L1_LANGS; lang++) {
        const char *t = l1_text(L1T_ST_ON, lang);
        CHECKF(strstr(t, "$1") && strstr(t, "$2") && strstr(t, "$3") && strstr(t, "$4"), "ST_ON lang %d lacks a placeholder", lang);
        char out[400];
        l1_expand(t, lang, out, sizeof out);
        CHECKF(!strchr(out, '$'), "ST_ON lang %d: placeholder left: %s", lang, out);
        CHECKF(strlen(out) > 20 && strlen(out) < 239, "ST_ON lang %d length %zu", lang, strlen(out));
    }
    /* stock words override the built-in ones */
    char w[4][64] = {"Weiter", "Laden", "Neu", "Kampagne X"};
    l1_set_stock_words(w);
    char out[400];
    l1_expand("a $1 b $2 c $3 d $4 e $5", 3, out, sizeof out);
    CHECK(!strcmp(out, "a Weiter b Laden c Neu d Kampagne X e $5"));
    l1_set_stock_words(NULL);
    l1_expand("$1", 3, out, sizeof out);
    CHECK(!strcmp(out, "Fortsetzen"));
    l1_expand("$4-$4-$4-$4-$4-$4-$4-$4-$4-$4-$4-$4", 0, out, 20);
    CHECK(strlen(out) <= 19);
    /* language tables */
    CHECK(l1_lang_from_code("rus") == 6 && l1_lang_from_code("chT") == 12 && l1_lang_from_code("xx") == -1 && l1_lang_from_code(NULL) == -1);
    CHECK(l1_lang_from_steam("english") == 0 && l1_lang_from_steam("schinese") == 11 && l1_lang_from_steam("brazilian") == 10 && l1_lang_from_steam("portuguese") == 9);
    CHECK(l1_lang_from_langid(0x0409) == 0 && l1_lang_from_langid(0x0809) == 0 && l1_lang_from_langid(0x040C) == 1 && l1_lang_from_langid(0x0C0C) == 1 && l1_lang_from_langid(0x0410) == 2);
    CHECK(l1_lang_from_langid(0x0407) == 3 && l1_lang_from_langid(0x0C0A) == 4 && l1_lang_from_langid(0x040A) == 4 && l1_lang_from_langid(0x0411) == 5 && l1_lang_from_langid(0x0419) == 6);
    CHECK(l1_lang_from_langid(0x0415) == 7 && l1_lang_from_langid(0x0413) == 8 && l1_lang_from_langid(0x0813) == 8 && l1_lang_from_langid(0x0816) == 9 && l1_lang_from_langid(0x0416) == 10);
    CHECK(l1_lang_from_langid(0x0804) == 11 && l1_lang_from_langid(0x1004) == 11 && l1_lang_from_langid(0x0404) == 12 && l1_lang_from_langid(0x0C04) == 11 && l1_lang_from_langid(0x1404) == 11);
    /* lang: the mod's language is the game's own for every LANGID (Hong Kong / Macau read the simplified texts in the game) and for every mLanguage */
    {
        int bad = 0;
        for (unsigned id = 0; id < 0x10000; id++) {
            int mine = l1_lang_from_langid(id), want = game_text_lang((unsigned)game_mlanguage(id));
            if ((mine < 0 ? 0 : mine) != want || l1_lang_from_game((unsigned)game_mlanguage(id)) != want) {
                if (bad++ < 5) printf("  LANGID %04x: mod %d, game %d\n", id, mine, want);
            }
        }
        CHECKF(bad == 0, "%d LANGIDs give another language than the game's", bad);
        for (unsigned m = 0; m < 22; m++) CHECKF(l1_lang_from_game(m) == game_text_lang(m), "mLanguage %u", m);
        CHECK(l1_lang_from_game(22) == -1 && l1_lang_from_game(0xFFFFFFFFu) == -1);
    }
    CHECK(l1_lang_from_langid(0x0422) == -1 && l1_lang_from_langid(0x0412) == -1 && l1_lang_from_langid(0) == -1);   /* Ukrainian, Korean: the game has no such text: English */
    CHECK(l1_lang_from_steam("dutch") == 8 && l1_lang_from_steam("klingon") == -1);
    for (int i = 0; i < L1_LANGS; i++) CHECK(l1_lang_from_code(l1_lang_codes[i]) == i);
}

static void test_persons(void)
{
    CHECK(l1_person_from_class("uPl< Jill >") == 0);
    CHECK(l1_person_from_class("uPl< JillEp1_2 >") == 0);
    CHECK(l1_person_from_class("uPl< ParkerLight >") == 2);
    CHECK(l1_person_from_class("uPl< ChrisDLC >") == 1);
    CHECK(l1_person_from_class("uPl< JessicaCold >") == 4);
    CHECK(l1_person_from_class("uPl< KeithDLC >") == 3);
    CHECK(l1_person_from_class("uPl< QuintLight >") == 5);
    CHECK(l1_person_from_class("uPl< Reymond >") == 6);
    CHECK(l1_person_from_class("uPlayer") == -1 && l1_person_from_class(NULL) == -1);
    CHECK(!strcmp(l1_person_name(0, 0), "Jill") && !strcmp(l1_person_name(2, 0), "Parker") && !strcmp(l1_person_name(9, 0), "?"));
    char names[7][48] = {"Джил Валентайн", "吉儿·范伦廷", "ジル・バレンタイン", "Keith Lumley", "", "Quint Cetcham ", "Raymond Vester"};
    l1_set_stock_names((const char(*)[48])names);
    CHECK(!strcmp(l1_person_name(0, 6), "Джил"));
    CHECK(!strcmp(l1_person_name(1, 11), "吉儿"));
    CHECK(!strcmp(l1_person_name(2, 5), "ジル"));
    CHECK(!strcmp(l1_person_name(3, 0), "Keith"));
    CHECK(!strcmp(l1_person_name(4, 0), "Jessica"));   /* empty stock entry: built-in fallback */
    CHECK(!strcmp(l1_person_name(5, 0), "Quint"));
    l1_set_stock_names(NULL);
    CHECK(!strcmp(l1_person_name(0, 6), "Jill"));
}

static l1_env mkenv(int lang, int pads)
{
    l1_env e;
    memset(&e, 0, sizeof e);
    e.lang = lang;
    e.pads_connected = pads;
    return e;
}

static void test_model(void)
{
    l1_t *l = l1_new();
    l1_settings s = {0, L1_LEAD, L1_LAYOUT_SIDE, 0};
    l1_env env = mkenv(0, 2);
    l1_open(l, &s, &env);
    l1_view v;
    l1_view_get(l, &v);
    CHECK(v.nrows == 7);
    CHECK(v.sel == 0);
    CHECK(!strcmp(v.title, "Co-Op Play"));
    CHECK(!v.cards_on);
    CHECK(v.status_kind == L1S_INFO);
    CHECK(v.rows[1].kind == L1V_GREY && v.rows[3].kind == L1V_GREY);   /* the rest is grey while the co-op is off */
    /* navigation skips the info rows */
    ev(l, L1E_DOWN); l1_view_get(l, &v); CHECK(v.sel == 1);
    ev(l, L1E_DOWN); l1_view_get(l, &v); CHECK(v.sel == 3);   /* row 2 (player 2) is info */
    ev(l, L1E_DOWN); l1_view_get(l, &v); CHECK(v.sel == 4);
    ev(l, L1E_DOWN); l1_view_get(l, &v); CHECK(v.sel == 6);   /* row 5 (pads found) is info */
    ev(l, L1E_DOWN); l1_view_get(l, &v); CHECK(v.sel == 0);   /* wraps */
    ev(l, L1E_UP); l1_view_get(l, &v); CHECK(v.sel == 6);
    ev(l, L1E_UP); l1_view_get(l, &v); CHECK(v.sel == 4);
    ev(l, L1E_UP); ev(l, L1E_UP); ev(l, L1E_UP);
    l1_view_get(l, &v); CHECK(v.sel == 0);
    /* arming */
    CHECK(!l1_changed(l));
    ev(l, L1E_RIGHT);
    l1_settings_get(l, &s);
    CHECK(s.armed == 1);
    CHECK(l1_changed(l));
    CHECK(!l1_changed(l));
    l1_view_get(l, &v);
    CHECK(v.cards_on && v.status_kind == L1S_OK);
    CHECK(strstr(v.status, "Continue") && strstr(v.status, "Load Game") && strstr(v.status, "New Game"));
    CHECK(strstr(v.card[0].chr, "Jill") && strstr(v.card[1].chr, "Parker"));
    CHECK(!strcmp(v.card[0].pad, "Gamepad 1") && !strcmp(v.card[1].pad, "Gamepad 2"));
    CHECK(v.rows[1].kind == L1V_NORMAL);
    /* role: partner */
    ev(l, L1E_DOWN);
    ev(l, L1E_OK);
    l1_settings_get(l, &s);
    CHECK(s.role == L1_PARTNER);
    l1_view_get(l, &v);
    CHECK(strstr(v.card[0].chr, "Parker") && strstr(v.card[1].chr, "Jill"));
    CHECK(strstr(v.rows[1].value, "Parker") && strstr(v.rows[1].value, "partner"));
    CHECK(strstr(v.rows[2].value, "Jill") && strstr(v.rows[2].value, "lead"));
    ev(l, L1E_LEFT);
    l1_settings_get(l, &s);
    CHECK(s.role == L1_LEAD);
    /* layout and pads */
    ev(l, L1E_DOWN);
    ev(l, L1E_RIGHT);
    l1_settings_get(l, &s);
    CHECK(s.layout == L1_LAYOUT_TOPBOTTOM);
    ev(l, L1E_DOWN);
    ev(l, L1E_OK);
    l1_settings_get(l, &s);
    CHECK(s.pads_swapped == 1);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.card[0].pad, "Gamepad 2") && !strcmp(v.card[1].pad, "Gamepad 1"));
    /* the assignment for the capture / split modules */
    l1_assign a;
    l1_assign_get(&s, &a);
    CHECK(a.unit1 == 0 && a.unit2 == 1 && a.pad1 == 0 && a.pad2 == 1 && a.swap == 1);
    s.role = L1_PARTNER, s.pads_swapped = 0;
    l1_assign_get(&s, &a);
    CHECK(a.unit1 == 1 && a.unit2 == 0 && a.pad1 == 0 && a.pad2 == 1 && a.swap == 0);
    /* Done and Back close */
    ev(l, L1E_DOWN);
    l1_view_get(l, &v);
    CHECK(v.sel == 6);
    CHECK(l1_take_request(l) == L1R_NONE);
    ev(l, L1E_OK);
    CHECK(l1_take_request(l) == L1R_CLOSE);
    CHECK(l1_take_request(l) == L1R_NONE);
    ev(l, L1E_BACK);
    CHECK(l1_take_request(l) == L1R_CLOSE);
    /* mouse */
    l1_open(l, &s, &env);
    { l1_event e = {L1E_HOVER, 3}; l1_event_in(l, &e); }
    l1_view_get(l, &v);
    CHECK(v.sel == 3);
    { l1_event e = {L1E_HOVER, 2}; l1_event_in(l, &e); }   /* info row: ignored */
    l1_view_get(l, &v);
    CHECK(v.sel == 3);
    { l1_event e = {L1E_CLICK, 6}; l1_event_in(l, &e); }
    CHECK(l1_take_request(l) == L1R_CLOSE);
    /* pad warnings only while armed */
    s.armed = 1;
    env = mkenv(0, 1);
    l1_open(l, &s, &env);
    l1_view_get(l, &v);
    CHECK(v.status_kind == L1S_WARN && strstr(v.status, "One gamepad"));
    env = mkenv(0, 0);
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(v.status_kind == L1S_WARN && strstr(v.status, "No gamepad"));
    env = mkenv(0, 2);
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(v.status_kind == L1S_OK);
    s.armed = 0;
    env = mkenv(0, 0);
    l1_open(l, &s, &env);
    l1_view_get(l, &v);
    CHECK(v.status_kind == L1S_INFO);
    /* known section names */
    s.armed = 1;
    s.role = L1_LEAD;
    env = mkenv(0, 2);
    snprintf(env.lead, sizeof env.lead, "Chris");
    snprintf(env.partner, sizeof env.partner, "Jessica");
    env.section_known = 1;
    l1_open(l, &s, &env);
    l1_view_get(l, &v);
    CHECK(strstr(v.card[0].chr, "Chris") && strstr(v.card[1].chr, "Jessica"));
    CHECK(!strstr(v.note2, "Episode 1"));
    env.section_known = 0;
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(strstr(v.note2, "Episode 1"));
    l1_free(l);
}

/* every language: every view string fits its buffer and is valid UTF-8 (no truncated multibyte character) */
static void test_views_all_languages(void)
{
    for (int lang = 0; lang < L1_LANGS; lang++)
        for (int armed = 0; armed < 2; armed++)
            for (int role = 0; role < 2; role++)
                for (int pads = 0; pads < 3; pads++) {
                    l1_t *l = l1_new();
                    l1_settings s = {armed, role, L1_LAYOUT_SIDE, 0};
                    l1_env env = mkenv(lang, pads);
                    l1_open(l, &s, &env);
                    l1_view v;
                    l1_view_get(l, &v);
                    CHECKF(utf8_ok(v.title) && utf8_ok(v.subtitle) && utf8_ok(v.status) && utf8_ok(v.hint) && utf8_ok(v.note) && utf8_ok(v.note2), "lang %d view text invalid UTF-8", lang);
                    for (int i = 0; i < v.nrows; i++) CHECKF(utf8_ok(v.rows[i].label) && utf8_ok(v.rows[i].value), "lang %d row %d invalid UTF-8", lang, i);
                    CHECKF(v.status[0] && strlen(v.status) < 230, "lang %d status length %zu", lang, strlen(v.status));
                    char h[300];
                    l1_hint_text(&s, &env, h, sizeof h);
                    CHECKF(utf8_ok(h) && h[0], "lang %d hint", lang);
                    l1_free(l);
                }
    l1_settings s = {1, L1_PARTNER, L1_LAYOUT_SIDE, 0};
    l1_env env = mkenv(0, 2);
    char h[300];
    l1_hint_text(&s, &env, h, sizeof h);
    CHECK(strstr(h, "Co-Op is ON") && strstr(h, "Parker / Jill") && strstr(h, "F6"));
    s.armed = 0;
    l1_hint_text(&s, &env, h, sizeof h);
    CHECK(!strcmp(h, "F6 / Back button: Co-Op Play"));
    l1_hint_text(&s, &env, h, 12);
    CHECK(strlen(h) == 11);   /* a short buffer truncates (the English hint is ASCII) */
}

static void test_ini(void)
{
    const char *path = "test_lobby1.ini";
    remove(path);
    l1_settings s = {0, L1_LEAD, L1_LAYOUT_SIDE, 0};
    int lang = -5;
    CHECK(!l1_ini_load(path, &s, &lang));
    CHECK(lang == -5);
    l1_settings w = {1, L1_PARTNER, L1_LAYOUT_TOPBOTTOM, 1};
    CHECK(l1_ini_save(path, &w, 6));
    l1_settings r = {0, L1_LEAD, L1_LAYOUT_SIDE, 0};
    lang = -1;
    CHECK(l1_ini_load(path, &r, &lang));
    CHECK(r.armed == 1 && r.role == L1_PARTNER && r.layout == L1_LAYOUT_TOPBOTTOM && r.pads_swapped == 1);
    CHECK(lang == 6);
    CHECK(l1_ini_save(path, &s, -1));
    lang = 4;
    CHECK(l1_ini_load(path, &r, &lang));
    CHECK(r.armed == 0 && r.role == L1_LEAD && r.layout == L1_LAYOUT_SIDE && r.pads_swapped == 0);
    CHECK(lang == 4);   /* no language line: untouched */
    /* garbage and out-of-range values */
    FILE *f = fopen(path, "wb");
    fputs("armed = 7\nrole=banana\nlayout=top\nnonsense\n=\n  pads = swapped  \nlanguage=zz\n", f);
    fclose(f);
    lang = 3;
    CHECK(l1_ini_load(path, &r, &lang));
    CHECK(r.armed == 1 && r.role == L1_LEAD && r.layout == L1_LAYOUT_TOPBOTTOM && r.pads_swapped == 1);
    CHECK(lang == -1);
    remove(path);
}

static void test_gmd(void)
{
    /* a tiny GMD built in memory (Rev1 layout v0x10201): header, name, 8-byte label records, labels, messages */
    unsigned char buf[600];
    memset(buf, 0, sizeof buf);
    memcpy(buf, "GMD\0", 4);
    *(unsigned *)(buf + 4) = 0x10201;
    *(unsigned *)(buf + 8) = 1;
    const char *labels = "char_name_00\0char_name_01\0";
    int lblen = 26;
    const char *msgs = "Jill Valentine\0Chris Redfield\0";
    int mlen = 30;
    *(unsigned *)(buf + 0x14) = 2;          /* labels */
    *(unsigned *)(buf + 0x18) = 2;          /* messages */
    *(unsigned *)(buf + 0x1C) = lblen;
    *(unsigned *)(buf + 0x20) = mlen;
    *(unsigned *)(buf + 0x24) = 4;          /* name length */
    memcpy(buf + 0x28, "Coop", 5);
    int off = 0x28 + 5;
    *(unsigned *)(buf + off) = 0, *(unsigned *)(buf + off + 4) = 0x1234;
    *(unsigned *)(buf + off + 8) = 1, *(unsigned *)(buf + off + 12) = 0x5678;
    off += 16;
    memcpy(buf + off, labels, lblen);
    off += lblen;
    memcpy(buf + off, msgs, mlen);
    off += mlen;
    gmd_t *g = gmd_parse(buf, (unsigned)off);
    CHECK(g != NULL);
    if (g) {
        CHECK(gmd_count(g) == 2);
        CHECK(!strcmp(gmd_by_label(g, "char_name_01"), "Chris Redfield"));
        CHECK(!strcmp(gmd_by_label(g, "char_name_00"), "Jill Valentine"));
        CHECK(gmd_by_label(g, "nope") == NULL);
        CHECK(!strcmp(gmd_by_index(g, 1), "Chris Redfield") && gmd_by_index(g, 2) == NULL);
        gmd_free(g);
    }
    CHECK(gmd_parse(buf, 20) == NULL);
    buf[0] = 'X';
    CHECK(gmd_parse(buf, (unsigned)off) == NULL);
}

static void test_input(void)
{
    l1_input in;
    l1_raw r;
    l1_event ev[8];
    int want = 0, n;
    memset(&r, 0, sizeof r);
    l1_input_reset(&in);
    /* closed: nothing happens, F6 opens (after the first neutral poll armed the input) */
    n = l1_input_poll(&in, &r, 1000, 0, ev, 8, &want);
    CHECK(n == 0 && !want);
    r.f6 = 1;
    r.enter = 1;   /* Enter is held too when the screen opens */
    n = l1_input_poll(&in, &r, 1010, 0, ev, 8, &want);
    CHECK(n == 0 && want == 1);
    n = l1_input_poll(&in, &r, 1020, 0, ev, 8, &want);   /* still held: no second open gesture */
    CHECK(n == 0 && want == 0);
    /* the lobby opens with F6 still held and Enter held too: neither counts */
    n = l1_input_poll(&in, &r, 1030, 1, ev, 8, &want);
    CHECK(n == 0);
    r.f6 = 0;
    n = l1_input_poll(&in, &r, 1040, 1, ev, 8, &want);
    CHECK(n == 0);
    r.enter = 0;
    n = l1_input_poll(&in, &r, 1050, 1, ev, 8, &want);
    CHECK(n == 0);
    /* navigation with repeat */
    r.down = 1;
    n = l1_input_poll(&in, &r, 1100, 1, ev, 8, &want);
    CHECK(n == 1 && ev[0].kind == L1E_DOWN);
    n = l1_input_poll(&in, &r, 1300, 1, ev, 8, &want);   /* before the first repeat (420 ms) */
    CHECK(n == 0);
    n = l1_input_poll(&in, &r, 1530, 1, ev, 8, &want);
    CHECK(n == 1 && ev[0].kind == L1E_DOWN);
    n = l1_input_poll(&in, &r, 1560, 1, ev, 8, &want);
    CHECK(n == 0);
    n = l1_input_poll(&in, &r, 1625, 1, ev, 8, &want);
    CHECK(n == 1);
    r.down = 0;
    /* confirm and back by keyboard and pad A / B; F6 closes */
    r.enter = 1; n = l1_input_poll(&in, &r, 2000, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_OK); r.enter = 0;
    r.esc = 1; n = l1_input_poll(&in, &r, 2010, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_BACK); r.esc = 0;
    n = l1_input_poll(&in, &r, 2020, 1, ev, 8, &want); CHECK(n == 0);
    r.pad_buttons[1] = 0x1000; n = l1_input_poll(&in, &r, 2030, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_OK); r.pad_buttons[1] = 0;
    r.pad_buttons[0] = 0x2000; n = l1_input_poll(&in, &r, 2040, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_BACK); r.pad_buttons[0] = 0;
    n = l1_input_poll(&in, &r, 2050, 1, ev, 8, &want); CHECK(n == 0);
    r.f6 = 1; n = l1_input_poll(&in, &r, 2060, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_BACK); r.f6 = 0;
    r.pad_buttons[0] = 0x0020; n = l1_input_poll(&in, &r, 2070, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_BACK); r.pad_buttons[0] = 0;
    /* d-pad and stick */
    n = l1_input_poll(&in, &r, 2100, 1, ev, 8, &want); CHECK(n == 0);
    r.pad_buttons[0] = 0x0001; n = l1_input_poll(&in, &r, 2110, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_UP); r.pad_buttons[0] = 0;
    n = l1_input_poll(&in, &r, 2120, 1, ev, 8, &want);
    r.lx[1] = 30000; n = l1_input_poll(&in, &r, 2130, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_RIGHT); r.lx[1] = -30000;
    n = l1_input_poll(&in, &r, 2140, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_LEFT); r.lx[1] = 0;
    r.ly[0] = -30000; n = l1_input_poll(&in, &r, 2150, 1, ev, 8, &want); CHECK(n == 1 && ev[0].kind == L1E_DOWN); r.ly[0] = 0;
    r.ly[0] = 1000; n = l1_input_poll(&in, &r, 2160, 1, ev, 8, &want); CHECK(n == 0);   /* below the dead zone */
    r.ly[0] = 0;
    /* the BACK button opens from the closed state, the held button does not count twice */
    l1_input_reset(&in);
    n = l1_input_poll(&in, &r, 3000, 0, ev, 8, &want); CHECK(!want);
    r.pad_buttons[1] = 0x0020; n = l1_input_poll(&in, &r, 3010, 0, ev, 8, &want); CHECK(want == 1);
    n = l1_input_poll(&in, &r, 3020, 0, ev, 8, &want); CHECK(want == 0);
    r.pad_buttons[1] = 0;
    /* a key that is already down when the module starts never opens */
    l1_input_reset(&in);
    r.f6 = 1; n = l1_input_poll(&in, &r, 4000, 0, ev, 8, &want); CHECK(!want);
    n = l1_input_poll(&in, &r, 4010, 0, ev, 8, &want); CHECK(!want);
    r.f6 = 0; n = l1_input_poll(&in, &r, 4020, 0, ev, 8, &want); CHECK(!want);
    r.f6 = 1; n = l1_input_poll(&in, &r, 4030, 0, ev, 8, &want); CHECK(want == 1);
    (void)n;
}

/* ---- network rows (env.net_rows = 1) ---- */
static void hover(l1_t *l, int row) { l1_event e = {L1E_HOVER, row, 0}; l1_event_in(l, &e); }
static void type_str(l1_t *l, const char *s)
{
    for (; *s; s++) {
        l1_event e = {L1E_CHAR, 0, (unsigned char)*s};
        l1_event_in(l, &e);
    }
}

static void test_addr(void)
{
    CHECK(l1_addr_ok("192.168.0.10:28410") == 1);
    CHECK(l1_addr_ok("my-pc.local") == 1 && l1_addr_ok("192.168.0.10:1") == 1 && l1_addr_ok("host:65535") == 1);
    CHECK(l1_addr_ok("host:0") == 0 && l1_addr_ok("host:65536") == 0 && l1_addr_ok("host:99999999999") == 0);
    CHECK(l1_addr_ok(":27015") == 0 && l1_addr_ok("") == 0 && l1_addr_ok(NULL) == 0);
    CHECK(l1_addr_ok("a b") == 0 && l1_addr_ok("1.2.3.4:") == 0 && l1_addr_ok("1.2.3.4:x") == 0 && l1_addr_ok("a:b:c") == 0);
    CHECK(l1_addr_ok("host/x") == 0 && l1_addr_ok("host:-1") == 0);
}

/* index of the row with this label in the view (-1 = not shown) */
static int row_of(const l1_view *v, const char *label)
{
    for (int i = 0; i < v->nrows; i++)
        if (!strcmp(v->rows[i].label, label)) return i;
    return -1;
}
static void click_row(l1_t *l, int row) { l1_event e = {L1E_CLICK, row, 0}; l1_event_in(l, &e); }

/* the product screen (env.net_rows = 1, lobby-must): one exclusive Mode row Off / Split screen / Network: host / Network: join, then only the rows of the mode */
static void test_net_model(void)
{
    l1_t *l = l1_new();
    l1_settings s = {0, L1_LEAD, L1_LAYOUT_SIDE, 0};
    l1_env env = mkenv(0, 2);
    env.net_rows = 1;
    l1_open(l, &s, &env);
    l1_view v;
    l1_view_get(l, &v);
    CHECK(v.nrows == 2 && !strcmp(v.rows[0].label, "Co-Op") && !strcmp(v.rows[0].value, "< Off >") && !strcmp(v.rows[1].label, "Done"));
    CHECK(!strcmp(v.subtitle, l1_text(L1T_ROW_HELP, 0)) && v.net_status[0] == 0 && !v.cards_on);
    CHECK(!strcmp(v.status, "Co-op is off. The game plays as usual."));
    /* Off -> Split screen: the split rows as before, the cards and the save note */
    ev(l, L1E_RIGHT);
    l1_settings_get(l, &s);
    CHECK(s.armed == 1 && s.net_mode == L1N_OFF && l1_changed(l));
    l1_view_get(l, &v);
    CHECK(v.nrows == 7 && !strcmp(v.rows[0].value, "< Split screen >") && !strcmp(v.rows[1].label, "Player 1 plays") && !strcmp(v.rows[3].label, "Screen layout"));
    CHECK(v.cards_on && v.status_kind == L1S_OK && strstr(v.note, "its own co-op save: your solo campaign save is not changed") && !strstr(v.note, "save slot") && !strcmp(v.subtitle, l1_text(L1T_SUBTITLE, 0)));   /* rev1 live 16: split co-op uses the co-op save (the text before said "your usual save slot") */
    CHECK(v.net_status[0] == 0);
    /* Split -> Network: host: the split is switched off (one mode only) */
    ev(l, L1E_RIGHT);
    l1_settings_get(l, &s);
    CHECK(s.armed == 0 && s.net_mode == L1N_HOST);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[0].value, "< Network: host >") && !strcmp(v.subtitle, l1_text(L1T_SUBTITLE_NET, 0)));
    CHECK(v.nrows == 8 && !strcmp(v.rows[1].label, "You play") && strstr(v.rows[1].value, "Jill (lead character)") && v.rows[1].kind == L1V_NORMAL);
    CHECK(!strcmp(v.rows[2].label, "Your address") && v.rows[2].kind == L1V_INFO && !strcmp(v.rows[2].value, "?"));
    CHECK(!strcmp(v.rows[3].label, "Start") && v.rows[3].kind == L1V_NORMAL && v.rows[4].kind == L1V_GREY && !strcmp(v.rows[7].label, "Done"));
    /* ep1-flow E12: the Downed / revive row after Connection, before Done: the host's choice, default on with 60 s */
    CHECK(!strcmp(v.rows[6].label, "Downed / revive") && !strcmp(v.rows[6].value, "< 60 s >") && v.rows[6].kind == L1V_NORMAL);
    /* round 2: the Way row (last before Done): Direct IP / Steam friends; the Steam way shows the friends-list hint instead of the address */
    CHECK(!strcmp(v.rows[5].label, "Connection") && !strcmp(v.rows[5].value, "< Direct IP >") && v.rows[5].kind == L1V_NORMAL);
    hover(l, 5);
    ev(l, L1E_RIGHT);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[5].value, "< Steam friends >") && strstr(v.rows[2].label, "Steam friends list") && v.rows[2].kind == L1V_INFO && !strcmp(v.rows[3].label, "Start"));
    {
        l1_settings w;
        l1_settings_get(l, &w);
        CHECK(w.way == 1);
    }
    ev(l, L1E_LEFT);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[5].value, "< Direct IP >") && !strcmp(v.rows[2].label, "Your address"));
    CHECK(row_of(&v, "Remove partner") < 0);   /* nobody to remove */
    CHECK(!strcmp(v.net_status, "Press Start: the other player joins with your address.") && !strcmp(v.status, v.net_status));
    CHECK(v.cards_on && !strcmp(v.card[0].who, "PLAYER 1") && !strcmp(v.card[0].chr, "Jill") && !strcmp(v.card[1].chr, "Parker") && !strcmp(v.card[1].pad, "Waiting for other player."));
    CHECK(strstr(v.note, "keeps its own save on each PC: your solo campaign save is not changed") && !strcmp(v.note2, l1_text(L1T_RAID_NOTE, 0)));   /* rev1 live 15: the co-op save */
    l1_env e2 = env;
    snprintf(e2.own_addr, sizeof e2.own_addr, "%s", "192.168.0.5:28410");
    l1_set_env(l, &e2);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[2].value, "192.168.0.5:28410"));
    /* the host chooses his character with the role row */
    hover(l, 1);
    ev(l, L1E_RIGHT);
    l1_settings_get(l, &s);
    l1_view_get(l, &v);
    CHECK(s.role == L1_PARTNER && strstr(v.rows[1].value, "Parker (partner)") && !strcmp(v.card[0].chr, "Parker"));
    ev(l, L1E_LEFT);
    l1_settings_get(l, &s);
    CHECK(s.role == L1_LEAD);
    /* Host -> Join: the address row takes the typed text, the role comes from the host */
    hover(l, 0);
    ev(l, L1E_RIGHT);
    l1_settings_get(l, &s);
    CHECK(s.net_mode == L1N_JOIN && s.armed == 0);
    l1_view_get(l, &v);
    CHECK(v.nrows == 8 && !strcmp(v.rows[1].label, "Host address") && strstr(v.rows[1].value, "type IP:port") && !strcmp(v.rows[2].label, "Start"));
    CHECK(!strcmp(v.rows[5].label, "Connection") && !strcmp(v.rows[7].label, "Done"));
    CHECK(!strcmp(v.rows[6].label, "Downed / revive") && !strcmp(v.rows[6].value, "-") && v.rows[6].kind == L1V_GREY);   /* ep1-flow: the guest sees the host's choice once connected */
    CHECK(!strcmp(v.rows[3].label, "You play") && v.rows[3].kind == L1V_GREY && !strcmp(v.rows[3].value, "-") && v.rows[4].kind == L1V_GREY);
    CHECK(!strcmp(v.net_status, "Press Start to connect to the host.") && !strcmp(v.card[0].who, "PLAYER 2") && !strcmp(v.card[0].chr, "-"));
    type_str(l, "9");   /* the mode row is selected: nothing typed */
    l1_settings_get(l, &s);
    CHECK(s.join_addr[0] == 0);
    hover(l, 1);
    type_str(l, " ");
    type_str(l, "192.168.0.10:28410");
    l1_settings_get(l, &s);
    CHECK(!strcmp(s.join_addr, "192.168.0.10:28410"));
    ev(l, L1E_BACKSPACE), ev(l, L1E_BACKSPACE);
    type_str(l, "10");
    l1_settings_get(l, &s);
    CHECK(!strcmp(s.join_addr, "192.168.0.10:28410"));
    /* a malformed address: the status says so, Start does nothing */
    for (int i = 0; i < 40; i++) ev(l, L1E_BACKSPACE);
    type_str(l, "bad:");
    l1_view_get(l, &v);
    CHECK(v.net_status_kind == L1S_WARN && !strcmp(v.net_status, "The address must look like 192.168.0.10:28410."));
    hover(l, 2);
    ev(l, L1E_OK);
    CHECK(l1_take_request(l) == L1R_NONE);
    hover(l, 1);
    for (int i = 0; i < 60; i++) type_str(l, "1");
    l1_settings_get(l, &s);
    CHECK(strlen(s.join_addr) == 47);
    for (int i = 0; i < 60; i++) ev(l, L1E_BACKSPACE);
    type_str(l, "10.0.0.5:28410");
    hover(l, 2);
    type_str(l, "zz");   /* the Start row is selected: no text */
    ev(l, L1E_OK);
    CHECK(l1_take_request(l) == L1R_NET_START);
    /* connecting: the address is named; the mode and the address are locked */
    l1_net_set_state(l, L1NS_CONNECTING);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[2].label, "Disconnect") && !strcmp(v.net_status, "Connecting to 10.0.0.5:28410...") && v.rows[0].kind == L1V_GREY && v.rows[1].kind == L1V_GREY);
    hover(l, 0);
    hover(l, 1);
    l1_view_get(l, &v);
    CHECK(v.sel == 2);
    /* refused: the reason in words, not "Connection failed" */
    l1_net_set_state(l, L1NS_ERROR);
    l1_net_set_reason(l, L1RS_FULL, NULL, NULL);
    l1_view_get(l, &v);
    CHECK(v.net_status_kind == L1S_WARN && !strcmp(v.net_status, "The session you are trying to join is already full.") && !strcmp(v.rows[2].label, "Start"));
    l1_net_set_reason(l, L1RS_MODVER, "0.1.1", "0.1.0");
    l1_view_get(l, &v);
    CHECK(!strcmp(v.net_status, "Co-op mod versions differ: host 0.1.1, you 0.1.0. Start the game from the Co-op shortcut to update it."));
    l1_net_set_reason(l, L1RS_NOANSWER, "10.0.0.5:28410", NULL);
    l1_view_get(l, &v);
    CHECK(!strncmp(v.net_status, "No answer from 10.0.0.5:28410.", 30));
    l1_net_set_reason(l, L1RS_NONE, NULL, NULL);
    l1_view_get(l, &v);
    CHECK(strstr(v.net_status, "Connection failed"));   /* a local failure without a reason: the generic text */
    /* a new attempt forgets the old reason */
    l1_net_set_reason(l, L1RS_STARTED, NULL, NULL);
    l1_net_set_state(l, L1NS_CONNECTING);
    CHECK(l1_net_reason(l) == L1RS_NONE);
    /* connected as the guest: "Joining as", the role from the host, ready flags */
    l1_net_set_state(l, L1NS_CONNECTED);
    l1_net_set_assigned(l, L1_PARTNER);
    l1_net_set_peer(l, "Lissa");
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[3].value, "Parker (partner)") && v.rows[3].kind == L1V_INFO && v.rows[4].kind == L1V_NORMAL);
    CHECK(!strcmp(v.net_status, "Joining as Parker (partner).  Mark yourself ready when you are set."));
    CHECK(!strcmp(v.card[0].chr, "Parker") && !strcmp(v.card[1].chr, "Jill") && !strcmp(v.card[1].pad, "Lissa (network)") && !strcmp(v.card[1].who, "PLAYER 1"));
    l1_net_set_peer_ready(l, 1);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.net_status, "Joining as Parker (partner).  Lissa is ready.  Mark yourself ready when you are set."));
    hover(l, 4);
    ev(l, L1E_OK);
    CHECK(l1_net_ready(l) == 1);
    l1_view_get(l, &v);
    CHECK(v.net_status_kind == L1S_OK && !strcmp(v.net_status, "Joining as Parker (partner).  Both players are ready."));
    /* START: what happens next; the ready row is locked */
    l1_net_set_started(l, 1);
    l1_view_get(l, &v);
    CHECK(v.net_status_kind == L1S_OK && !strcmp(v.net_status, "Waiting for Lissa to start the story. Your game follows automatically.") && v.rows[4].kind == L1V_GREY);
    /* the host leaves: the reason, the started state ends */
    l1_net_set_state(l, L1NS_ERROR);
    l1_net_set_reason(l, L1RS_HOST_ENDED, "Lissa", NULL);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.net_status, "The host has ended the session.") && v.rows[0].kind == L1V_NORMAL);
    l1_net_set_state(l, L1NS_IDLE);
    CHECK(l1_net_reason(l) == L1RS_NONE);
    l1_free(l);

    /* the host: partner joined / ready, Remove partner with the game's own question, START text, the partner leaves */
    l = l1_new();
    l1_settings h = {0, L1_LEAD, L1_LAYOUT_SIDE, 0, L1N_HOST, ""};
    l1_open(l, &h, &env);
    l1_net_set_state(l, L1NS_WAIT);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.net_status, "Waiting for the other player to join...") && !strcmp(v.note2, l1_text(L1T_RAID_NOTE, 0)));
    env.alone_s = 31;   /* nobody came for 30 s: the internet hint */
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(strstr(v.note2, "UDP port 28410"));
    env.alone_s = 0;
    l1_set_env(l, &env);
    l1_net_set_state(l, L1NS_CONNECTED);
    l1_net_set_peer(l, "Bob");
    l1_view_get(l, &v);
    CHECK(!strcmp(v.net_status, "Bob has joined.  Mark yourself ready when you are set."));
    int kr = row_of(&v, "Remove partner");
    CHECK(kr == 5 && v.nrows == 9 && v.rows[4].kind == L1V_NORMAL);
    hover(l, kr);
    ev(l, L1E_OK);
    CHECK(l1_take_request(l) == L1R_NONE);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[kr].label, "Do you want to kick Bob?") && !strcmp(v.rows[kr].value, "OK"));
    ev(l, L1E_BACK);   /* Back takes the question back, the lobby stays */
    CHECK(l1_take_request(l) == L1R_NONE);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[kr].label, "Remove partner"));
    ev(l, L1E_OK), ev(l, L1E_UP);   /* moving away takes it back too */
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[kr].label, "Remove partner"));
    hover(l, kr);
    ev(l, L1E_OK), ev(l, L1E_OK);
    CHECK(l1_take_request(l) == L1R_NET_KICK);
    ev(l, L1E_LEFT);   /* Left / Right never remove anybody */
    CHECK(l1_take_request(l) == L1R_NONE);
    l1_net_set_peer_ready(l, 1);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.net_status, "Bob has joined.  Bob is ready.  Mark yourself ready when you are set."));
    l1_net_set_ready(l, 1);
    l1_net_set_started(l, 1);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.net_status, "Start Episode 1: Campaign > Continue or New Game. Bob follows you automatically.") && row_of(&v, "Remove partner") < 0);
    CHECK(v.rows[1].kind == L1V_GREY && v.rows[1].action == 0);   /* the roles are final */
    l1_net_set_state(l, L1NS_WAIT);
    l1_net_set_reason(l, L1RS_PARTNER_LEFT, "Bob", NULL);
    l1_view_get(l, &v);
    CHECK(v.net_status_kind == L1S_WARN && !strcmp(v.net_status, "Bob has left.  Waiting for the other player to join..."));
    /* a save outside Episode 1: the host is told before the start */
    env.save_area = 0x91030100u;
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(strstr(v.note2, "Episode 1") && strstr(v.note2, "New Game") && strstr(v.note2, "Load Game"));
    env.save_area = 0x91010200u;
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(!strstr(v.note2, "New Game"));
    /* rev1 live 16: the host's co-op save holds the roles it was played with: the room says that Continue plays them, New Game the room's choice */
    env.coop_role1 = 2;
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.note2, "Continue of the co-op save keeps its characters: you play Parker, your partner Jill. New Game keeps the choice made here."));
    env.coop_role1 = 1;
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(strstr(v.note2, "you play Jill, your partner Parker") != NULL);
    env.coop_role1 = 0;
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(!strstr(v.note2, "co-op save"));
    l1_free(l);

    /* a stale armed=1 with a network mode (console, old ini): the network wins, never both */
    l = l1_new();
    l1_settings both = {1, L1_LEAD, L1_LAYOUT_SIDE, 0, L1N_JOIN, ""};
    l1_open(l, &both, &env);
    l1_settings_get(l, &s);
    CHECK(s.armed == 0 && s.net_mode == L1N_JOIN && l1_mode_of(&s) == L1M_JOIN);
    l1_net_adopt(l, L1N_HOST, NULL);
    l1_settings_get(l, &s);
    CHECK(s.armed == 0 && l1_mode_of(&s) == L1M_HOST);
    l1_mode_set(&s, L1M_SPLIT);
    CHECK(s.armed == 1 && s.net_mode == L1N_OFF && l1_mode_of(&s) == L1M_SPLIT);
    l1_mode_set(&s, L1M_OFF);
    CHECK(s.armed == 0 && s.net_mode == L1N_OFF);
    l1_free(l);
    /* the mode wraps: Off <- Left = Network: join */
    l = l1_new();
    s = (l1_settings){0, L1_LEAD, L1_LAYOUT_SIDE, 0};
    l1_open(l, &s, &env);
    ev(l, L1E_LEFT);
    l1_settings_get(l, &s);
    CHECK(l1_mode_of(&s) == L1M_JOIN);
    click_row(l, 0);
    l1_settings_get(l, &s);
    CHECK(l1_mode_of(&s) == L1M_OFF);
    l1_free(l);
    /* with env.net_rows = 0 the screen is the local one, exactly as before */
    l = l1_new();
    s.net_mode = L1N_HOST;
    env.net_rows = 0;
    l1_open(l, &s, &env);
    l1_view_get(l, &v);
    CHECK(v.nrows == 7 && v.net_status[0] == 0 && !strcmp(v.rows[0].label, "Co-Op"));
    l1_free(l);
}

/* every language, mode, link state and flag: the strings are valid UTF-8 and fit; no placeholder is left */
static void test_net_all_languages(void)
{
    for (int lang = 0; lang < L1_LANGS; lang++)
        for (int mode = 0; mode < 4; mode++)
            for (int st = 0; st < 5; st++)
                for (int flags = 0; flags < 16; flags++) {
                    l1_t *l = l1_new();
                    l1_settings s = {0, L1_LEAD, L1_LAYOUT_SIDE, 0, 0, "192.168.0.10:28410"};
                    l1_mode_set(&s, mode);
                    l1_env env = mkenv(lang, 2);
                    env.net_rows = 1;
                    env.alone_s = flags & 4 ? 40 : 0;
                    env.save_area = flags & 8 ? 0x91050100u : 0;
                    snprintf(env.own_addr, sizeof env.own_addr, "%s", "10.0.0.2:28410");
                    l1_open(l, &s, &env);
                    l1_net_set_state(l, st);
                    l1_net_set_peer(l, "ÄÖÜ-Name-with-24-chars!!");
                    l1_net_set_assigned(l, L1_PARTNER);
                    l1_net_set_peer_ready(l, flags & 1);
                    l1_net_set_reason(l, (flags * 7 + st) % L1RS_COUNT, "0.1.1-long-ver", "0.1.0-long-ver");
                    if (flags & 2) l1_net_set_ready(l, 1), l1_net_set_started(l, flags & 1);
                    l1_view v;
                    l1_view_get(l, &v);
                    CHECKF(v.nrows >= 2 && v.nrows <= 9, "lang %d mode %d: rows %d", lang, mode, v.nrows);
                    CHECKF(utf8_ok(v.net_status) && strlen(v.net_status) < sizeof v.net_status - 1, "lang %d mode %d state %d: status %zu bytes", lang, mode, st, strlen(v.net_status));
                    CHECKF((mode >= L1M_HOST) == (v.net_status[0] != 0), "lang %d mode %d state %d: network line %s", lang, mode, st, v.net_status);
                    CHECKF(!strchr(v.net_status, '$') && !strstr(v.net_status, "%s") && !strchr(v.note2, '$'), "lang %d mode %d: placeholder left: %s / %s", lang, mode, v.net_status, v.note2);
                    CHECKF(utf8_ok(v.status) && utf8_ok(v.note) && utf8_ok(v.note2) && utf8_ok(v.subtitle) && strlen(v.note2) < sizeof v.note2 - 1, "lang %d mode %d notes", lang, mode);
                    for (int c = 0; c < 2; c++) CHECKF(utf8_ok(v.card[c].pad) && utf8_ok(v.card[c].chr) && utf8_ok(v.card[c].who), "lang %d card %d", lang, c);
                    for (int i = 0; i < v.nrows; i++) {
                        CHECKF(utf8_ok(v.rows[i].label) && utf8_ok(v.rows[i].value), "lang %d row %d invalid UTF-8", lang, i);
                        CHECKF(strlen(v.rows[i].value) < 120 && strlen(v.rows[i].label) < 90, "lang %d row %d too long", lang, i);
                    }
                    l1_free(l);
                }
}

/* the game's texts (Coop_<lang>.gmd): the tags go, <USER> becomes the name, line breaks a space (none in Japanese / Chinese); a broken one keeps the English */
static void test_stock_texts(void)
{
    char out[256];
    l1_stock_clean("<SIZE 18><USER><COL FFFFFFFF> has left.\r\n</COL></SIZE>", 0, out, sizeof out);
    CHECK(!strcmp(out, "%s has left."));
    l1_stock_clean("<SIZE 18><COL FFFFFFFF>\"</COL><USER><COL FFFFFFFF>\" est parti.</COL></SIZE>", 1, out, sizeof out);
    CHECK(!strcmp(out, "\"%s\" est parti."));
    l1_stock_clean("La session que vous avez essayé\r\nde rejoindre est pleine.", 1, out, sizeof out);
    CHECK(!strcmp(out, "La session que vous avez essayé de rejoindre est pleine."));
    l1_stock_clean("セッションはすでに満員で\r\n参加できませんでした", 5, out, sizeof out);
    CHECK(!strcmp(out, "セッションはすでに満員で参加できませんでした"));
    l1_stock_clean("A sessão que você está tentando entrar\r\n\xE2\x80\xA8j\xC3\xA1 est\xC3\xA1 cheia.", 10, out, sizeof out);
    CHECK(!strcmp(out, "A sessão que você está tentando entrar já está cheia."));
    l1_stock_clean("De sessie waaraan je wilt deel-\r\nnemen is al vol.", 8, out, sizeof out);
    CHECK(!strcmp(out, "De sessie waaraan je wilt deelnemen is al vol."));
    l1_stock_clean("100% <USER> and <USER> %d", 0, out, sizeof out);   /* a stray % is doubled, a second <USER> dropped: never a printf format */
    CHECK(!strcmp(out, "100%% %s and %%d"));
    l1_stock_clean("Ожидание других игроков.", 6, out, 10);   /* a short buffer never cuts a character in half */
    CHECK(utf8_ok(out) && strlen(out) <= 9);
    l1_stock_clean("no end <SIZE", 0, out, sizeof out);
    CHECK(!strcmp(out, "no end"));
    /* loaded texts replace the English, a text with a format of its own is refused */
    CHECK(!strcmp(l1_stock(L1K_LEFT), "%s has left."));
    l1_set_stock_text(L1K_LEFT, "%s вышел.");
    CHECK(!strcmp(l1_stock(L1K_LEFT), "%s вышел."));
    l1_set_stock_text(L1K_LEFT, "%d bad");
    CHECK(!strcmp(l1_stock(L1K_LEFT), "%s has left."));
    l1_set_stock_text(L1K_LEFT, "%s %s twice");
    CHECK(!strcmp(l1_stock(L1K_LEFT), "%s has left."));
    l1_set_stock_text(L1K_LEFT, NULL);
    CHECK(!strcmp(l1_stock(L1K_LEFT), "%s has left.") && l1_stock(99)[0] == 0);
    for (int i = 0; i < L1K_COUNT; i++) CHECK(l1_stock_labels[i] && l1_stock_labels[i][0] && l1_stock(i)[0]);
    /* l1_fmt: only %s and %%, the arguments in order, UTF-8 whole at the cut */
    l1_fmt(out, sizeof out, "a %s b %s c %s %d 100%%", "1", "2");
    CHECK(!strcmp(out, "a 1 b 2 c  %d 100%"));
    l1_fmt(out, 8, "%s", "Привет", NULL);
    CHECK(utf8_ok(out) && !strcmp(out, "При"));
    l1_fmt(out, sizeof out, NULL, "x", NULL);
    CHECK(out[0] == 0);
}

/* the in-game strip: every kind in every language names the partner, has no placeholder left and fits */
static void test_strips(void)
{
    for (int lang = 0; lang < L1_LANGS; lang++)
        for (int k = L1G_NONE; k < L1G_COUNT; k++) {
            l1_strip_in in = {k, "Lissa", 37};
            char out[400];
            int kind = -1;
            l1_strip_text(&in, lang, out, sizeof out, &kind);
            CHECKF(utf8_ok(out) && strlen(out) < sizeof out - 1 && !strstr(out, "%s") && !strchr(out, '$'), "strip %d lang %d: %s", k, lang, out);
            CHECKF((k == L1G_NONE) == (out[0] == 0), "strip %d lang %d empty", k, lang);
            if (k == L1G_WAIT_LOAD || k == L1G_SILENT || k == L1G_PARTNER_LEFT || k == L1G_GUEST_HOLD || k == L1G_HOST_GO || k == L1G_SOLO_WATCH || k == L1G_CONT_ROLES) CHECKF(strstr(out, "Lissa") != NULL, "strip %d lang %d lacks the name: %s", k, lang, out);
            if (k == L1G_SILENT) CHECKF(strstr(out, "37") != NULL, "strip %d lang %d lacks the seconds", k, lang);
            CHECKF(kind == L1S_INFO || kind == L1S_WARN, "strip %d kind %d", k, kind);
        }
    char out[400];
    l1_strip_in in = {L1G_HOST_LOST, "", 0};
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "Connection to the other player has been terminated.  Returning to the title screen..."));
    in.kind = L1G_CONT_ROLES, snprintf(in.name, sizeof in.name, "Parker");   /* rev1 live 16 */
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "Continue keeps the characters of the co-op save. Your character: Parker."));
    l1_strip_text(&in, 6, out, sizeof out, NULL);
    CHECK(strstr(out, "Parker") && strstr(out, "«") && !strchr(out, '$'));
    in.name[0] = 0;
    in.kind = L1G_PARTNER_LEFT;
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "PLAYER 2 has left.  Co-op has ended; your game goes on."));
    in.kind = L1G_SILENT, in.seconds = 18, snprintf(in.name, sizeof in.name, "Bob");
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "No answer from Bob for 18 s (game in the background, loading or stalled)..."));
    l1_strip_text(NULL, 0, out, sizeof out, NULL);
    CHECK(out[0] == 0);
    /* every reason has a line, in every language */
    for (int lang = 0; lang < L1_LANGS; lang++)
        for (int r = L1RS_FAILED; r < L1RS_COUNT; r++) {
            l1_reason_text(r, lang, "a1", "a2", out, sizeof out);
            CHECKF(out[0] && utf8_ok(out) && !strstr(out, "%s"), "reason %d lang %d", r, lang);
        }
    /* the placeholders of the translated texts: the same count as English in every language */
    static const int one[] = {L1T_NET_PEER, L1T_JOINING_AS, L1T_NET_ST_CONNECTING_TO, L1T_NET_ST_GO_HOST, L1T_NET_ST_GO_GUEST, L1T_REF_MODVER_OLD, L1T_REF_NOANSWER, L1T_WAIT_LOAD};
    static const int two[] = {L1T_REF_MODVER, L1T_PARTNER_SILENT};
    for (int lang = 0; lang < L1_LANGS; lang++) {
        for (unsigned i = 0; i < sizeof one / sizeof one[0]; i++) {
            const char *t = l1_text(one[i], lang), *p = strstr(t, "%s");
            CHECKF(p && !strstr(p + 2, "%s"), "text %d lang %d: one %%s", one[i], lang);
        }
        for (unsigned i = 0; i < sizeof two / sizeof two[0]; i++) {
            const char *t = l1_text(two[i], lang), *p = strstr(t, "%s");
            CHECKF(p && strstr(p + 2, "%s") && !strstr(strstr(p + 2, "%s") + 2, "%s"), "text %d lang %d: two %%s", two[i], lang);
        }
        CHECKF(strstr(l1_text(L1T_NET_ST_GO_HOST, lang), "$1") && strstr(l1_text(L1T_NET_ST_GO_HOST, lang), "$3") && strstr(l1_text(L1T_NET_ST_GO_HOST, lang), "$4"), "GO_HOST lang %d", lang);
        CHECKF(strstr(l1_text(L1T_NET_EP_ONLY, lang), "$2") && strstr(l1_text(L1T_NET_EP_ONLY, lang), "$3"), "EP_ONLY lang %d", lang);
    }
}

/* ready1 (2026-11-09, the owner: "Ready" as in Jericho, whose seats read "<name>: <character> - READY"): in a network room each player card shows its player's ready
 * state with the game's own Raid lobby words ("Ready" / "Waiting"), un-ready goes back, the START keeps both "Ready", a partner who leaves takes his line with him;
 * and the ready gate's strip (menu.c holds the title menu's Campaign until both are ready) says what is missing in every language. The model before ready1 had no
 * ready line on the cards (only the status line of this PC's view) and no gate strip. */
static void test_ready_cards(void)
{
    l1_t *l = l1_new();
    l1_settings h = {0, L1_LEAD, L1_LAYOUT_SIDE, 0, L1N_HOST, ""};
    l1_env env = mkenv(0, 2);
    env.net_rows = 1;
    l1_open(l, &h, &env);
    l1_view v;
    l1_view_get(l, &v);
    CHECK(v.cards_on && v.card[0].ready_on == 0 && v.card[1].ready_on == 0 && v.card[0].ready[0] == 0);   /* no link: no ready line */
    l1_net_set_state(l, L1NS_WAIT);
    l1_view_get(l, &v);
    CHECK(v.card[0].ready_on == 0 && v.card[1].ready_on == 0 && !strcmp(v.card[1].pad, "Waiting for other player."));   /* nobody yet */
    l1_net_set_state(l, L1NS_CONNECTED);
    l1_net_set_peer(l, "Bob");
    l1_view_get(l, &v);
    CHECK(v.card[0].ready_on == 1 && !strcmp(v.card[0].ready, "Waiting") && v.card[1].ready_on == 1 && !strcmp(v.card[1].ready, "Waiting"));
    CHECK(!strcmp(v.card[1].pad, "Bob (network)") && !strcmp(v.card[0].who, "PLAYER 1") && !strcmp(v.card[1].who, "PLAYER 2"));
    l1_net_set_peer_ready(l, 1);   /* the partner marks ready: HIS card says so, this PC's card does not change */
    l1_view_get(l, &v);
    CHECK(v.card[1].ready_on == 2 && !strcmp(v.card[1].ready, "Ready") && v.card[0].ready_on == 1 && !strcmp(v.card[0].ready, "Waiting"));
    l1_net_set_peer_ready(l, 0);   /* ... and takes it back (un-ready) */
    l1_view_get(l, &v);
    CHECK(v.card[1].ready_on == 1 && !strcmp(v.card[1].ready, "Waiting"));
    int rr = row_of(&v, "I am ready");
    CHECK(rr >= 0 && v.rows[rr].kind == L1V_NORMAL);
    hover(l, rr);
    ev(l, L1E_OK);   /* this player marks ready with the row */
    l1_view_get(l, &v);
    CHECK(l1_net_ready(l) == 1 && v.card[0].ready_on == 2 && !strcmp(v.card[0].ready, "Ready") && !strcmp(v.rows[rr].value, "< On >"));
    ev(l, L1E_OK);   /* un-ready */
    l1_view_get(l, &v);
    CHECK(l1_net_ready(l) == 0 && v.card[0].ready_on == 1 && !strcmp(v.rows[rr].value, "< Off >"));
    ev(l, L1E_OK);
    l1_net_set_peer_ready(l, 1);
    l1_view_get(l, &v);
    CHECK(v.card[0].ready_on == 2 && v.card[1].ready_on == 2 && v.net_status_kind == L1S_OK && strstr(v.net_status, "Both players are ready."));
    l1_net_set_started(l, 1);   /* the host's link started the session: both stay "Ready", the ready row is locked */
    l1_view_get(l, &v);
    CHECK(v.card[0].ready_on == 2 && v.card[1].ready_on == 2 && v.rows[rr].kind == L1V_GREY);
    l1_net_set_started(l, 0);
    l1_net_set_state(l, L1NS_WAIT);   /* the partner leaves: his line goes, the flags are cleared (the model clears both when the link leaves CONNECTED) */
    l1_net_set_reason(l, L1RS_PARTNER_LEFT, "Bob", NULL);
    l1_view_get(l, &v);
    CHECK(v.card[0].ready_on == 0 && v.card[1].ready_on == 0 && l1_net_ready(l) == 0 && !strcmp(v.card[1].pad, "Waiting for other player."));
    l1_net_set_state(l, L1NS_CONNECTED);   /* a new partner: both start "Waiting" */
    l1_view_get(l, &v);
    CHECK(v.card[0].ready_on == 1 && v.card[1].ready_on == 1);
    /* the game's words of the running language (Coop_rus.gmd partner_ready / partner_recruit) */
    l1_set_stock_text(L1K_PARTNER_READY, "\xD0\x93\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xBE");
    l1_set_stock_text(L1K_PARTNER_WAIT, "\xD0\x9E\xD0\xB6\xD0\xB8\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5");
    l1_net_set_peer_ready(l, 1);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.card[1].ready, "\xD0\x93\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xBE") && !strcmp(v.card[0].ready, "\xD0\x9E\xD0\xB6\xD0\xB8\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5"));
    l1_set_stock_text(L1K_PARTNER_READY, NULL);
    l1_set_stock_text(L1K_PARTNER_WAIT, NULL);
    CHECK(!strcmp(l1_stock_labels[L1K_PARTNER_READY], "partner_ready") && !strcmp(l1_stock_labels[L1K_PARTNER_WAIT], "partner_recruit") && !strcmp(l1_stock_banks[L1K_PARTNER_WAIT], "Coop"));
    l1_free(l);
    /* the guest's room: its own card first, the host's second; the split screen never has a ready line */
    l = l1_new();
    l1_settings g = {0, L1_LEAD, L1_LAYOUT_SIDE, 0, L1N_JOIN, "10.0.0.5:28410"};
    l1_open(l, &g, &env);
    l1_net_set_state(l, L1NS_CONNECTED);
    l1_net_set_assigned(l, L1_PARTNER);
    l1_net_set_peer(l, "Lissa");
    l1_net_set_peer_ready(l, 1);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.card[0].who, "PLAYER 2") && v.card[0].ready_on == 1 && !strcmp(v.card[1].pad, "Lissa (network)") && v.card[1].ready_on == 2);
    l1_free(l);
    l = l1_new();
    l1_settings sp = {1, L1_LEAD, L1_LAYOUT_SIDE, 0};
    l1_open(l, &sp, &env);
    l1_view_get(l, &v);
    CHECK(v.cards_on && v.card[0].ready_on == 0 && v.card[1].ready_on == 0);
    l1_free(l);

    /* the ready gate's strip: what is missing, then where to mark ready */
    char out[400];
    int kind = -1;
    l1_strip_in in = {L1G_READY_GATE, "Bob", 0, L1GF_HOST};
    l1_strip_text(&in, 0, out, sizeof out, &kind);
    CHECK(!strcmp(out, "Waiting for the other player to join...  F6 / Back button: Co-Op Play") && kind == L1S_WARN);
    in.flags = 0;   /* a guest still connecting */
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "Connecting to the host...  F6 / Back button: Co-Op Play"));
    in.flags = L1GF_HOST | L1GF_CONNECTED;
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "Mark yourself ready when you are set.  F6 / Back button: Co-Op Play"));
    in.flags = L1GF_CONNECTED | L1GF_PEER_READY;
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "Bob is ready.  Mark yourself ready when you are set.  F6 / Back button: Co-Op Play"));
    in.flags = L1GF_HOST | L1GF_CONNECTED | L1GF_READY;
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "You are ready. Waiting for the other player..."));
    in.flags = L1GF_CONNECTED | L1GF_READY | L1GF_PEER_READY;
    l1_strip_text(&in, 0, out, sizeof out, NULL);
    CHECK(!strcmp(out, "Both players are ready."));
    for (int lang = 0; lang < L1_LANGS; lang++)
        for (int f = 0; f < 16; f++) {
            l1_strip_in gi = {L1G_READY_GATE, "Lissa", 0, f};
            l1_strip_text(&gi, lang, out, sizeof out, &kind);
            CHECKF(out[0] && utf8_ok(out) && strlen(out) < sizeof out - 1 && !strstr(out, "%s") && !strchr(out, '$') && kind == L1S_WARN, "gate strip lang %d flags %d: %s", lang, f, out);
            if ((f & L1GF_CONNECTED) && (f & L1GF_PEER_READY) && !(f & L1GF_READY)) CHECKF(strstr(out, "Lissa") != NULL, "gate strip lang %d flags %d lacks the name: %s", lang, f, out);
        }
}

static void test_net_ini(void)
{
    const char *path = "test_lobby1_net.ini";
    remove(path);
    l1_settings w = {1, L1_LEAD, L1_LAYOUT_SIDE, 0, L1N_JOIN, "10.0.0.5:27015"};
    CHECK(l1_ini_save(path, &w, -1));
    l1_settings r = {0, L1_LEAD, L1_LAYOUT_SIDE, 0, L1N_OFF, ""};
    CHECK(l1_ini_load(path, &r, NULL));
    CHECK(!strcmp(r.join_addr, "10.0.0.5:27015"));
    CHECK(r.net_mode == L1N_OFF);   /* the network mode is not persisted */
    CHECK(w.net_mode == L1N_JOIN);
    remove(path);
}

static void test_net_input(void)
{
    l1_input in;
    l1_raw r;
    l1_event evs[8];
    int want = 0, n;
    memset(&r, 0, sizeof r);
    l1_input_reset(&in);
    n = l1_input_poll(&in, &r, 100, 1, evs, 8, &want);   /* neutral: the input is armed */
    CHECK(n == 0);
    r.typed[0] = 'a'; r.typed[1] = '1'; r.typed[2] = ' '; r.typed[3] = 0x80; r.typed[4] = '.';
    n = l1_input_poll(&in, &r, 110, 1, evs, 8, &want);
    /* printable ASCII goes to the model (space included: the model drops it); the 0x80 byte is not ASCII */
    CHECK(n == 4 && evs[0].kind == L1E_CHAR && evs[0].ch == 'a' && evs[1].kind == L1E_CHAR && evs[1].ch == '1' && evs[2].ch == ' ' && evs[3].ch == '.');
    memset(r.typed, 0, sizeof r.typed);
    n = l1_input_poll(&in, &r, 120, 1, evs, 8, &want);
    CHECK(n == 0);
    /* the characters typed while the lobby is closed are dropped */
    r.typed[0] = 'x';
    n = l1_input_poll(&in, &r, 130, 0, evs, 8, &want);
    CHECK(n == 0);
    memset(r.typed, 0, sizeof r.typed);
    /* backspace: an edge, then auto repeat */
    r.backspace = 1;
    n = l1_input_poll(&in, &r, 200, 1, evs, 8, &want);
    CHECK(n == 1 && evs[0].kind == L1E_BACKSPACE);
    n = l1_input_poll(&in, &r, 300, 1, evs, 8, &want);
    CHECK(n == 0);
    n = l1_input_poll(&in, &r, 630, 1, evs, 8, &want);
    CHECK(n == 1 && evs[0].kind == L1E_BACKSPACE);
    r.backspace = 0;
    n = l1_input_poll(&in, &r, 640, 1, evs, 8, &want);
    CHECK(n == 0);
    /* the model takes the typed characters the same way the old events are taken */
    l1_t *l = l1_new();
    l1_settings s = {0, L1_LEAD, L1_LAYOUT_SIDE, 0};
    l1_env env = mkenv(0, 2);
    env.net_rows = 1;
    l1_open(l, &s, &env);
    r.typed[0] = '1';   /* the selection is on the Mode row: the typed text goes nowhere */
    n = l1_input_poll(&in, &r, 700, 1, evs, 8, &want);
    CHECK(n == 1 && evs[0].kind == L1E_CHAR);
    for (int i = 0; i < n; i++) l1_event_in(l, &evs[i]);
    l1_settings_get(l, &s);
    CHECK(s.join_addr[0] == 0);
    l1_free(l);
}

/* the WM_CHAR queue of the glue (menu.c): printable ASCII in order, the rest dropped, bounded, taken at most 7 at a time */
static void test_typed_queue(void)
{
    l1_typed q;
    unsigned char out[8];
    memset(&q, 0, sizeof q);
    CHECK(l1_typed_take(&q, out) == 0 && out[0] == 0);
    l1_typed_push(&q, 0x08); l1_typed_push(&q, 0x0D); l1_typed_push(&q, 0x1B); l1_typed_push(&q, 0xE9); l1_typed_push(&q, 0x7F);   /* backspace, enter, esc, non-ASCII, DEL */
    CHECK(q.count == 0);
    l1_typed_push(&q, '1'); l1_typed_push(&q, 0x20); l1_typed_push(&q, '.'); l1_typed_push(&q, '~');
    CHECK(q.count == 4);
    CHECK(l1_typed_take(&q, out) == 4 && !strcmp((const char *)out, "1 .~") && q.count == 0);
    /* more than 7 at once: 7 are taken, the rest waits for the next pass */
    for (int i = 0; i < 20; i++) l1_typed_push(&q, 'a' + (i % 26));
    CHECK(q.count == 20);
    CHECK(l1_typed_take(&q, out) == 7 && out[7] == 0 && out[0] == 'a' && out[6] == 'g' && q.count == 13);
    /* the queue is a ring: the head moves past the end of the buffer */
    CHECK(l1_typed_take(&q, out) == 7 && out[0] == 'h' && q.count == 6);
    CHECK(l1_typed_take(&q, out) == 6 && out[0] == 'o' && out[5] == 't' && q.count == 0);
    /* full (32): the newest characters are dropped, the old ones are kept in order */
    for (int i = 0; i < 40; i++) l1_typed_push(&q, '0' + (i % 10));
    CHECK(q.count == 32);
    CHECK(l1_typed_take(&q, out) == 7 && !strcmp((const char *)out, "0123456"));
    CHECK(q.count == 25);
}

/* the stub of the network glue (menu.c net_request): Start -> error state, Disconnect -> idle; the model keeps asking for Start after an error */
static void test_net_stub(void)
{
    l1_t *l = l1_new();
    l1_settings s = {1, L1_LEAD, L1_LAYOUT_SIDE, 0, L1N_JOIN, "10.0.0.7:27015"};
    l1_env env = mkenv(0, 2);
    env.net_rows = 1;
    l1_open(l, &s, &env);
    l1_view v;
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[2].label, "Start") && v.rows[2].action != 0);
    hover(l, 2);
    ev(l, L1E_OK);
    CHECK(l1_take_request(l) == L1R_NET_START);
    l1_net_set_state(l, L1NS_ERROR);   /* what the stub does with a Start */
    l1_view_get(l, &v);
    CHECK(v.net_status_kind == L1S_WARN && strstr(v.net_status, "Connection failed"));
    CHECK(!strcmp(v.rows[2].label, "Start") && v.sel == 2);
    ev(l, L1E_OK);   /* Start again: the error state is not a link, so the request comes again */
    CHECK(l1_take_request(l) == L1R_NET_START);
    l1_net_set_state(l, L1NS_IDLE);
    l1_view_get(l, &v);
    CHECK(v.net_status_kind == L1S_INFO && !strcmp(v.net_status, "Press Start to connect to the host."));
    /* the address stays after the error; the local screen is not touched by the stub */
    l1_settings_get(l, &s);
    CHECK(!strcmp(s.join_addr, "10.0.0.7:27015"));
    l1_free(l);
}

/* ep1-flow E12: the Downed / revive row (host chooses, the guest sees it) and its session flags byte; E5: the end-of-Episode-1 strips */
static void test_downed_row(void)
{
    /* the flags byte: bit 0 the rule, bits 1..2 the bleed-out preset = Revelations 2's (downed_logic.h dl_cfg_from_flags) */
    static const int vals[L1D_N] = {L1D_OFF, L1D_30, L1D_60, L1D_120, L1D_NEVER};
    static const unsigned want[L1D_N] = {0u, 3u, 1u, 5u, 7u};
    static const unsigned bleed[L1D_N] = {0u, 30000u, 60000u, 120000u, 0u};
    for (int i = 0; i < L1D_N; i++) {
        unsigned f = l1_downed_flags(vals[i]);
        CHECKF(f == want[i], "downed %d -> flags %u (want %u)", vals[i], f, want[i]);
        CHECKF(l1_downed_from_flags(f) == vals[i], "flags %u back to %d", f, l1_downed_from_flags(f));
        dl_cfg c;
        dl_cfg_from_flags(&c, f);
        CHECKF(c.enabled == (vals[i] != L1D_OFF) && (!c.enabled || c.bleed_ms == bleed[i]), "downed %d: rule %d bleed %u", vals[i], c.enabled, (unsigned)c.bleed_ms);
    }
    CHECK(l1_downed_flags(99) == 1u && l1_downed_from_flags(0x7Eu) == L1D_OFF && l1_downed_from_flags(0xF9u) == L1D_60);   /* other bits do not change the reading */
    /* every value has a text in every language */
    for (int lang = 0; lang < L1_LANGS; lang++)
        for (int i = 0; i < L1D_N; i++) {
            char t[96];
            l1_downed_text(vals[i], lang, t, sizeof t);
            CHECKF(t[0] && utf8_ok(t) && !strchr(t, '%'), "downed text %d lang %d: %s", vals[i], lang, t);
        }
    char t[96];
    l1_downed_text(L1D_30, 5, t, sizeof t);
    CHECK(!strcmp(t, "30秒"));
    l1_downed_text(L1D_120, 6, t, sizeof t);
    CHECK(!strcmp(t, "120 с"));
    l1_downed_text(L1D_OFF, 6, t, sizeof t);
    CHECK(!strcmp(t, l1_text(L1T_OFF, 6)));   /* the game's word not loaded: the built-in */
    l1_set_stock_text(L1K_OFF, "ВЫКЛ.");     /* the game's options word (option_rus.gmd game_setting_menu_01_02) comes first */
    l1_downed_text(L1D_OFF, 6, t, sizeof t);
    CHECK(!strcmp(t, "ВЫКЛ.") && l1_stock_have(L1K_OFF));
    l1_set_stock_text(L1K_OFF, NULL);
    CHECK(!l1_stock_have(L1K_OFF) && !strcmp(l1_stock(L1K_OFF), "Off") && !strcmp(l1_stock_banks[L1K_OFF], "option") && !strcmp(l1_stock_banks[L1K_EPISODE1], "campaign"));
    CHECK(!strcmp(l1_stock_labels[L1K_OFF], "game_setting_menu_01_02") && !strcmp(l1_stock_labels[L1K_EPISODE1], "episode_01") && !strcmp(l1_stock_banks[L1K_FULL], "Coop"));

    /* the host's row: default 60 s, Right cycles 120 s, no time limit, Off, 30 s, 60 s; Left goes back; locked after the START */
    l1_t *l = l1_new();
    l1_settings s;
    memset(&s, 0, sizeof s);   /* a zeroed record (no ini) = the default: on, 60 s */
    l1_env env = mkenv(0, 1);
    env.net_rows = 1;
    l1_open(l, &s, &env);
    ev(l, L1E_RIGHT), ev(l, L1E_RIGHT);   /* Mode: Off -> Split screen -> Network: host */
    l1_view v;
    l1_view_get(l, &v);
    int dr = row_of(&v, "Downed / revive");
    CHECK(dr == 6 && !strcmp(v.rows[dr].value, "< 60 s >") && v.rows[dr].kind == L1V_NORMAL && !strcmp(v.rows[dr + 1].label, "Done"));
    static const char *const cyc[] = {"< 120 s >", "< No time limit >", "< Off >", "< 30 s >", "< 60 s >"};
    static const int cycv[] = {L1D_120, L1D_NEVER, L1D_OFF, L1D_30, L1D_60};
    hover(l, dr);
    for (int i = 0; i < 5; i++) {
        ev(l, L1E_RIGHT);
        l1_view_get(l, &v);
        l1_settings_get(l, &s);
        CHECKF(!strcmp(v.rows[dr].value, cyc[i]) && s.downed == cycv[i] && l1_changed(l), "downed right %d: %s (%d)", i, v.rows[dr].value, s.downed);
    }
    ev(l, L1E_LEFT);
    l1_settings_get(l, &s);
    CHECK(s.downed == L1D_30);
    ev(l, L1E_OK);   /* OK steps on like Right */
    l1_settings_get(l, &s);
    CHECK(s.downed == L1D_60);
    l1_net_set_state(l, L1NS_WAIT);
    l1_net_set_state(l, L1NS_CONNECTED);
    l1_view_get(l, &v);
    CHECK(v.rows[row_of(&v, "Downed / revive")].kind == L1V_NORMAL);   /* connected, before the START: still the host's to choose */
    l1_net_set_started(l, 1);
    l1_view_get(l, &v);
    dr = row_of(&v, "Downed / revive");
    CHECK(dr >= 0 && v.rows[dr].kind == L1V_INFO && !strcmp(v.rows[dr].value, "60 s"));   /* final after the START */
    hover(l, dr);
    ev(l, L1E_RIGHT);
    l1_settings_get(l, &s);
    CHECK(s.downed == L1D_60);
    l1_free(l);

    /* the guest's row: the host's choice from the session record, grey until connected */
    l = l1_new();
    memset(&s, 0, sizeof s);
    l1_mode_set(&s, L1M_JOIN);
    l1_open(l, &s, &env);
    l1_net_set_host_flags(l, 3);
    l1_view_get(l, &v);
    dr = row_of(&v, "Downed / revive");
    CHECK(dr == 6 && v.rows[dr].kind == L1V_GREY && !strcmp(v.rows[dr].value, "-"));
    l1_net_set_state(l, L1NS_CONNECTING);
    l1_net_set_state(l, L1NS_CONNECTED);
    l1_view_get(l, &v);
    CHECK(v.rows[dr].kind == L1V_INFO && !strcmp(v.rows[dr].value, "30 s"));
    l1_net_set_host_flags(l, 0);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[dr].value, "Off"));
    l1_net_set_host_flags(l, 7);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[dr].value, "No time limit"));
    l1_net_set_host_flags(l, -1);
    l1_view_get(l, &v);
    CHECK(v.rows[dr].kind == L1V_GREY && !strcmp(v.rows[dr].value, "-"));
    hover(l, dr);
    ev(l, L1E_RIGHT);   /* the guest cannot change it */
    l1_settings_get(l, &s);
    CHECK(s.downed == L1D_60 && !l1_changed(l));
    l1_free(l);
    /* the split screen page has no such row (the local rule stays the lab's REV1_DOWNED=1) */
    l = l1_new();
    memset(&s, 0, sizeof s);
    l1_mode_set(&s, L1M_SPLIT);
    l1_open(l, &s, &env);
    l1_view_get(l, &v);
    CHECK(row_of(&v, "Downed / revive") < 0);
    l1_free(l);

    /* persisted: downed=off|30|60|120|never; a missing key or a stray value = 60 s */
    const char *path = "test_lobby1_downed.ini";
    for (int i = 0; i < L1D_N; i++) {
        remove(path);
        l1_settings w;
        memset(&w, 0, sizeof w);
        w.downed = vals[i];
        CHECK(l1_ini_save(path, &w, -1));
        l1_settings r;
        memset(&r, 0, sizeof r);
        r.downed = 77;
        CHECK(l1_ini_load(path, &r, NULL));
        CHECKF(r.downed == vals[i], "ini downed %d -> %d", vals[i], r.downed);
    }
    FILE *f = fopen(path, "wb");
    if (f) fputs("armed=0\r\ndowned=banana\r\n", f), fclose(f);
    l1_settings r;
    memset(&r, 0, sizeof r);
    r.downed = L1D_OFF;
    CHECK(l1_ini_load(path, &r, NULL) && r.downed == L1D_60);
    f = fopen(path, "wb");
    if (f) fputs("armed=0\r\n", f), fclose(f);
    r.downed = L1D_30;
    CHECK(l1_ini_load(path, &r, NULL) && r.downed == L1D_30);   /* no key: the value stays as it was */
    remove(path);

    /* E5 strips: "Episode 1" is the game's own word when loaded, the language's built-in otherwise; the guest that was behind is told it goes back to the title */
    for (int lang = 0; lang < L1_LANGS; lang++) {
        l1_strip_in in = {L1G_EP1_DONE, "Bob", 0};
        char o[400];
        int k = -1;
        l1_strip_text(&in, lang, o, sizeof o, &k);
        CHECKF(strstr(o, l1_text(L1T_EPISODE1, lang)) && !strstr(o, "%s") && k == L1S_INFO, "ep1 done lang %d: %s", lang, o);
        in.kind = L1G_EP1_BEHIND;
        l1_strip_text(&in, lang, o, sizeof o, &k);
        CHECKF(strstr(o, l1_text(L1T_EPISODE1, lang)) && strstr(o, l1_text(L1T_TO_TITLE, lang)) && k == L1S_WARN, "ep1 behind lang %d: %s", lang, o);
    }
    l1_set_stock_text(L1K_EPISODE1, "第1章");
    l1_strip_in in = {L1G_EP1_DONE, "", 0};
    char o[400];
    l1_strip_text(&in, 0, o, sizeof o, NULL);
    CHECK(!strcmp(o, "第1章 complete! The co-op session has ended; your game goes on alone."));
    l1_set_stock_text(L1K_EPISODE1, NULL);
    l1_strip_text(&in, 6, o, sizeof o, NULL);
    CHECK(!strcmp(o, "Эпизод 1 пройден! Совместная сессия завершена, дальше вы играете один."));
}

/* rel010: the friend row (Join + Steam friends): the Steam friends in the game with a lobby; left / right picks, OK / click joins the one shown with his SteamID as the
 * join target (L1R_NET_START, as if typed). Not in Host, not in Direct IP, not while a link runs; the rows above keep their places (the lab scripts click by index). */
static void test_friend_row(void)
{
    l1_t *l = l1_new();
    l1_settings s;
    memset(&s, 0, sizeof s);
    s.way = 1;   /* Steam friends */
    l1_env env = mkenv(0, 1);
    env.net_rows = 1;
    l1_open(l, &s, &env);
    ev(l, L1E_RIGHT), ev(l, L1E_RIGHT), ev(l, L1E_RIGHT);   /* Mode: Off -> Split screen -> Network: host -> Network: join */
    l1_view v;
    l1_view_get(l, &v);
    int fr = row_of(&v, "Friend hosting");
    CHECKF(fr == 7 && v.rows[fr].kind == L1V_GREY && !strcmp(v.rows[fr].value, "no friend is hosting now") && !strcmp(v.rows[fr + 1].label, "Done"),
           "friend row without friends: index %d kind %d value %s", fr, fr >= 0 ? v.rows[fr].kind : -1, fr >= 0 ? v.rows[fr].value : "");
    CHECK(row_of(&v, "Start") == 2 && row_of(&v, "Connection") == 5 && row_of(&v, "Downed / revive") == 6);   /* the rows above keep their places */
    /* two friends hosting */
    env.n_friends = 2;
    snprintf(env.friend_name[0], sizeof env.friend_name[0], "Friend A");
    snprintf(env.friend_id[0], sizeof env.friend_id[0], "76561198000000042");
    snprintf(env.friend_name[1], sizeof env.friend_name[1], "Other");
    snprintf(env.friend_id[1], sizeof env.friend_id[1], "76561198000000043");
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    fr = row_of(&v, "Friend hosting");
    CHECKF(fr == 7 && v.rows[fr].kind == L1V_NORMAL && !strcmp(v.rows[fr].value, "< Friend A >"), "friend row: %s", fr >= 0 ? v.rows[fr].value : "-");
    hover(l, fr);
    ev(l, L1E_RIGHT);
    l1_view_get(l, &v);
    CHECKF(!strcmp(v.rows[fr].value, "< Other >"), "friend row right: %s", v.rows[fr].value);
    CHECK(l1_take_request(l) == L1R_NONE);   /* picking is not joining */
    ev(l, L1E_LEFT);
    ev(l, L1E_OK);
    l1_settings_get(l, &s);
    CHECKF(!strcmp(s.join_addr, "76561198000000042") && l1_take_request(l) == L1R_NET_START && l1_changed(l), "friend row OK: join %s", s.join_addr);
    /* a click on the row does the same */
    l1_event e = {L1E_CLICK, fr, 0};
    l1_event_in(l, &e);
    CHECK(l1_take_request(l) == L1R_NET_START);
    /* one friend: no arrows */
    env.n_friends = 1;
    l1_set_env(l, &env);
    l1_view_get(l, &v);
    CHECK(!strcmp(v.rows[fr].value, "Friend A"));
    /* while the link runs (connecting) the row is gone, so is it in Direct IP and in Host */
    l1_net_set_state(l, L1NS_CONNECTING);
    l1_view_get(l, &v);
    CHECK(row_of(&v, "Friend hosting") < 0);
    l1_net_set_state(l, L1NS_IDLE);
    s.way = 0;
    l1_free(l);
    l = l1_new();
    l1_open(l, &s, &env);
    ev(l, L1E_RIGHT), ev(l, L1E_RIGHT), ev(l, L1E_RIGHT);
    l1_view_get(l, &v);
    CHECK(row_of(&v, "Friend hosting") < 0);   /* Direct IP */
    ev(l, L1E_LEFT);
    l1_view_get(l, &v);
    CHECK(row_of(&v, "Friend hosting") < 0);   /* Host */
    l1_free(l);
    /* every language has both texts */
    for (int lang = 0; lang < L1_LANGS; lang++)
        CHECKF(utf8_ok(l1_text(L1T_NET_FRIEND, lang)) && utf8_ok(l1_text(L1T_NET_NOFRIEND, lang)) && (lang == 0 || strcmp(l1_text(L1T_NET_FRIEND, lang), l1_text(L1T_NET_FRIEND, 0))),
               "friend texts lang %d", lang);
}

int main(void)
{
    test_typed_queue();
    test_net_stub();
    test_texts();
    test_persons();
    test_model();
    test_views_all_languages();
    test_ini();
    test_gmd();
    test_input();
    test_addr();
    test_net_model();
    test_net_all_languages();
    test_stock_texts();
    test_strips();
    test_ready_cards();
    test_net_ini();
    test_net_input();
    test_downed_row();
    test_friend_row();
    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "FAILED\n" : "ALL OK\n");
    return fails ? 1 : 0;
}
