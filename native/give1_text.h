/*
 * give1_text.h - the texts of the "give to partner" entry (give1.h) in the 13 languages of the game. Column order (lobby1.h l1_lang_codes): English, French, Italian,
 * German, Spanish, Japanese, Russian, Polish, Dutch, Portuguese (Portugal), Portuguese (Brazil), Chinese (Simplified), Chinese (Traditional).
 *
 * Words of the game (knowledge\i18n-style.md rule 1, read from work\game\nativePC\msg\*_<lang>.gmd): the partner = msgCommon common_demand_b / gameover_hint
 * coop_hint_28 (Partner, partenaire, partner, compañero, パートナー, партнёр (the mod writes ё), partner, partner, parceiro, 同伴); the full-bag line follows the
 * stock myBag error_item_over ("You cannot hold any more."); the action follows the stock item memo cockpit item_memo_01 ("<B_DE> Read": an infinitive / plain
 * form, de / ru / zh write the icon without a space). The decide icon <B_DE> and the stock "<B_CA> Close" are the game's own (the close string is read from the
 * cockpit bank at run time). Every CJK character is in the game's Font_chS / Font_chT / Font_eng (jpn) (tests/test_give1.c checks it when the game folder is found).
 */
#ifndef GIVE1_TEXT_H
#define GIVE1_TEXT_H

#include <stdio.h>
#include <string.h>

#define GV_LANGS 13
enum { GV_T_GIVE = 0, GV_T_FULL, GV_T_NOW, GV_T_COUNT };

static const char *const GV_TEXT[GV_T_COUNT][GV_LANGS] = {
    [GV_T_GIVE] = {"Give to partner", "Donner au partenaire", "Dai al partner", "Dem Partner geben", "Dar al compañero", "パートナーに渡す", "Отдать партнёру",
                   "Daj partnerowi", "Aan partner geven", "Dar ao parceiro", "Dar ao parceiro", "交给同伴", "交給同伴"},
    [GV_T_FULL] = {"Your partner cannot hold any more.", "L'inventaire de votre partenaire est plein.", "Il tuo partner non ha più spazio.",
                   "Ihr Partner kann nichts mehr aufnehmen.", "Tu compañero no puede llevar más.", "パートナーはそれ以上所持できません", "Партнёр несёт максимальный вес.",
                   "Twój partner nie uniesie nic więcej.", "Je partner kan er geen meer vasthouden.", "O seu parceiro não pode carregar mais.",
                   "Seu parceiro não pode carregar mais.", "同伴无法拥有更多。", "同伴無法擁有更多。"},
    [GV_T_NOW] = {"Your partner cannot take it now.", "Votre partenaire ne peut pas le prendre.", "Il tuo partner ora non può prenderlo.",
                  "Ihr Partner kann es jetzt nicht annehmen.", "Tu compañero no puede cogerlo ahora.", "パートナーは今は受け取れません", "Партнёр сейчас не может это взять.",
                  "Twój partner nie może teraz tego wziąć.", "Je partner kan het nu niet aannemen.", "O seu parceiro não o pode receber agora.",
                  "Seu parceiro não pode receber agora.", "同伴现在无法接收。", "同伴現在無法接收。"},
};

/* the stock item memo writes the decide icon without a space in German, Russian and Chinese (cockpit item_memo_01 "<B_DE>Lesen", "<B_DE>Читать", "<B_DE>读") */
static inline int gv_icon_tight(int lang) { return lang == 3 || lang == 6 || lang == 11 || lang == 12; }

static inline const char *gv_text(int id, int lang)
{
    if (id < 0 || id >= GV_T_COUNT) return "";
    if (lang < 0 || lang >= GV_LANGS || !GV_TEXT[id][lang]) lang = 0;
    return GV_TEXT[id][lang];
}

/* the action half of the memo bar: "<B_DE> Give to partner" (the stock "<B_DE> Read" pattern). Returns the length written. */
static inline int gv_memo_action(char *out, int size, int lang)
{
    int n = snprintf(out, (size_t)size, "<B_DE>%s%s", gv_icon_tight(lang) ? "" : " ", gv_text(GV_T_GIVE, lang));
    return n < 0 ? 0 : n >= size ? size - 1 : n;
}

/* visible characters of a game string (UTF-8 code points outside <...> tags): the length limits of the tests */
static inline int gv_visible_cps(const char *s)
{
    int n = 0, tag = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '<') tag = 1;
        else if (*p == '>' && tag) tag = 0;
        else if (!tag && (*p & 0xC0) != 0x80) n++;
    }
    return n;
}

#endif
