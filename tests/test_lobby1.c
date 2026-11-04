/*
 * Offline test of the Revelations 1 co-op lobby model (native/lobby1.c, lobby1_text.c, gmdtext.c). Native Windows / any host build, no game needed.
 *   sh tests/build.sh && tests/build/test_lobby1.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/gmdtext.h"
#include "../native/lobby1.h"

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

static void test_texts(void)
{
    for (int id = 0; id < L1T_COUNT; id++)
        for (int lang = 0; lang < L1_LANGS; lang++) {
            const char *s = l1_text(id, lang);
            CHECKF(s && s[0], "text %d lang %d empty", id, lang);
            CHECKF(utf8_ok(s), "text %d lang %d is not valid UTF-8", id, lang);
            CHECKF(strlen(s) < 280, "text %d lang %d too long (%zu)", id, lang, strlen(s));
            if (lang && id != L1T_PADS && id != L1T_PAD1 && id != L1T_PAD2 && id != L1T_PARTNER)   /* these words are the same in some languages */
                CHECKF(strcmp(s, l1_text(id, 0)) != 0, "text %d lang %d equals English", id, lang);
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

int main(void)
{
    test_texts();
    test_persons();
    test_model();
    test_views_all_languages();
    test_ini();
    test_gmd();
    test_input();
    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "FAILED\n" : "ALL OK\n");
    return fails ? 1 : 0;
}
