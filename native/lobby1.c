/*
 * lobby1.c - model of the Revelations 1 co-op lobby (see lobby1.h). Pure C: no Windows, no game memory.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lobby1.h"

enum { A_NONE = 0, A_ARMED, A_ROLE, A_P2, A_LAYOUT, A_PADS, A_PADINFO, A_DONE };

struct l1 {
    l1_settings s;
    l1_env env;
    int sel;
    int request;
    int changed;
    int open;
};

/* ---------------------------------------------------------------- person names */
static const char *const k_person_en[7] = {"Jill", "Chris", "Parker", "Keith", "Jessica", "Quint", "Raymond"};
static char g_stock_names[7][48];

/* first name of a full name: up to the first space, middle dot or katakana middle dot */
static void first_name(const char *full, char *out, size_t size)
{
    size_t n = 0;
    for (const char *p = full; *p && n + 1 < size; p++) {
        if (*p == ' ') break;
        if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB7) break;                                           /* U+00B7 */
        if ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x83 && (unsigned char)p[2] == 0xBB) break;            /* U+30FB */
        out[n++] = *p;
    }
    out[n] = 0;
}

void l1_set_stock_names(const char names[7][48])
{
    for (int i = 0; i < 7; i++) {
        g_stock_names[i][0] = 0;
        if (names && names[i][0]) first_name(names[i], g_stock_names[i], sizeof g_stock_names[i]);
    }
}

const char *l1_person_name(int person, int lang)
{
    (void)lang;
    if (person < 0 || person > 6) return "?";
    return g_stock_names[person][0] ? g_stock_names[person] : k_person_en[person];
}

static int ci_contains(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

int l1_person_from_class(const char *cls)
{
    if (!cls) return -1;
    static const struct { const char *key; int person; } map[] = {{"Jill", 0}, {"Chris", 1}, {"Parker", 2}, {"Keith", 3}, {"Jessica", 4}, {"Quint", 5}, {"Reymond", 6}, {"Raymond", 6}};
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (ci_contains(cls, map[i].key)) return map[i].person;
    return -1;
}

/* ---------------------------------------------------------------- assignment */
void l1_assign_get(const l1_settings *s, l1_assign *a)
{
    a->unit1 = s->role == L1_PARTNER ? 1 : 0;
    a->unit2 = 1 - a->unit1;
    a->pad1 = 0;
    a->pad2 = 1;
    a->swap = s->pads_swapped ? 1 : 0;
}

/* ---------------------------------------------------------------- model */
l1_t *l1_new(void)
{
    l1_t *l = calloc(1, sizeof *l);
    if (l) {
        l->s.role = L1_LEAD;
        l->s.layout = L1_LAYOUT_SIDE;
    }
    return l;
}
void l1_free(l1_t *l) { free(l); }

static void sanitize(l1_settings *s)
{
    s->armed = s->armed != 0;
    s->role = s->role == L1_PARTNER ? L1_PARTNER : L1_LEAD;
    s->layout = s->layout == L1_LAYOUT_TOPBOTTOM ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE;
    s->pads_swapped = s->pads_swapped != 0;
}

void l1_open(l1_t *l, const l1_settings *s, const l1_env *env)
{
    if (s) l->s = *s;
    sanitize(&l->s);
    if (env) l->env = *env;
    if (l->env.lang < 0 || l->env.lang >= L1_LANGS) l->env.lang = 0;
    l->sel = 0;
    l->request = L1R_NONE;
    l->changed = 0;
    l->open = 1;
}

void l1_set_env(l1_t *l, const l1_env *env)
{
    if (!env) return;
    l->env = *env;
    if (l->env.lang < 0 || l->env.lang >= L1_LANGS) l->env.lang = 0;
}

static const char *lead_name(const l1_t *l) { return l->env.lead[0] ? l->env.lead : l1_person_name(0, l->env.lang); }
static const char *partner_name(const l1_t *l) { return l->env.partner[0] ? l->env.partner : l1_person_name(2, l->env.lang); }

static int build_rows(const l1_t *l, l1_row *rows)
{
    int lang = l->env.lang, n = 0;
    char who1[120], who2[120];
    const char *p1 = l->s.role == L1_PARTNER ? partner_name(l) : lead_name(l);
    const char *p2 = l->s.role == L1_PARTNER ? lead_name(l) : partner_name(l);
    snprintf(who1, sizeof who1, "%s (%s)", p1, l1_text(l->s.role == L1_PARTNER ? L1T_PARTNER : L1T_LEAD, lang));
    snprintf(who2, sizeof who2, "%s (%s)", p2, l1_text(l->s.role == L1_PARTNER ? L1T_LEAD : L1T_PARTNER, lang));
#define ROW(act, knd, lab, val) do { snprintf(rows[n].label, sizeof rows[n].label, "%s", (lab)); snprintf(rows[n].value, sizeof rows[n].value, "%s", (val)); rows[n].kind = (knd); rows[n].action = (act); n++; } while (0)
    char v[160];
    snprintf(v, sizeof v, "< %s >", l1_text(l->s.armed ? L1T_ON : L1T_OFF, lang));
    ROW(A_ARMED, L1V_NORMAL, l1_text(L1T_COOP, lang), v);
    int g = l->s.armed ? L1V_NORMAL : L1V_GREY;
    snprintf(v, sizeof v, "< %s >", who1);
    ROW(A_ROLE, g, l1_text(L1T_P1PLAYS, lang), v);
    ROW(A_P2, l->s.armed ? L1V_INFO : L1V_GREY, l1_text(L1T_P2PLAYS, lang), who2);
    snprintf(v, sizeof v, "< %s >", l1_text(l->s.layout == L1_LAYOUT_TOPBOTTOM ? L1T_TOPBOTTOM : L1T_SIDE, lang));
    ROW(A_LAYOUT, g, l1_text(L1T_LAYOUT, lang), v);
    snprintf(v, sizeof v, "< %s >", l1_text(l->s.pads_swapped ? L1T_PADS_SWAPPED : L1T_PADS_NORMAL, lang));
    ROW(A_PADS, g, l1_text(L1T_PADS, lang), v);
    snprintf(v, sizeof v, "%d", l->env.pads_connected);
    ROW(A_PADINFO, l->s.armed && l->env.pads_connected < 2 ? L1V_NORMAL : L1V_INFO, l1_text(L1T_PADS_FOUND, lang), v);
    ROW(A_DONE, L1V_NORMAL, l1_text(L1T_DONE, lang), "");
#undef ROW
    return n;
}

static int selectable(int action) { return action == A_ARMED || action == A_ROLE || action == A_LAYOUT || action == A_PADS || action == A_DONE; }

static void step(l1_t *l, int dir)
{
    l1_row rows[L1_ROWS];
    int n = build_rows(l, rows);
    for (int i = 0; i < n; i++) {
        l->sel = (l->sel + dir + n) % n;
        if (selectable(rows[l->sel].action)) return;
    }
}

static void change(l1_t *l, int action, int dir)
{
    (void)dir;   /* every setting has two values: left, right and confirm all toggle */
    switch (action) {
    case A_ARMED: l->s.armed = !l->s.armed; l->changed = 1; break;
    case A_ROLE: l->s.role = l->s.role == L1_LEAD ? L1_PARTNER : L1_LEAD; l->changed = 1; break;
    case A_LAYOUT: l->s.layout = l->s.layout == L1_LAYOUT_SIDE ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE; l->changed = 1; break;
    case A_PADS: l->s.pads_swapped = !l->s.pads_swapped; l->changed = 1; break;
    default: break;
    }
}

void l1_event_in(l1_t *l, const l1_event *e)
{
    l1_row rows[L1_ROWS];
    int n = build_rows(l, rows);
    if (l->sel < 0 || l->sel >= n) l->sel = 0;
    switch (e->kind) {
    case L1E_UP: step(l, -1); break;
    case L1E_DOWN: step(l, 1); break;
    case L1E_LEFT: change(l, rows[l->sel].action, -1); break;
    case L1E_RIGHT: change(l, rows[l->sel].action, 1); break;
    case L1E_OK:
        if (rows[l->sel].action == A_DONE) l->request = L1R_CLOSE;
        else change(l, rows[l->sel].action, 1);
        break;
    case L1E_BACK: l->request = L1R_CLOSE; break;
    case L1E_HOVER:
        if (e->row >= 0 && e->row < n && selectable(rows[e->row].action)) l->sel = e->row;
        break;
    case L1E_CLICK:
        if (e->row >= 0 && e->row < n && selectable(rows[e->row].action)) {
            l->sel = e->row;
            if (rows[e->row].action == A_DONE) l->request = L1R_CLOSE;
            else change(l, rows[e->row].action, 1);
        }
        break;
    default: break;
    }
}

int l1_take_request(l1_t *l)
{
    int r = l->request;
    l->request = L1R_NONE;
    if (r == L1R_CLOSE) l->open = 0;
    return r;
}

int l1_changed(l1_t *l)
{
    int c = l->changed;
    l->changed = 0;
    return c;
}

void l1_settings_get(l1_t *l, l1_settings *s) { *s = l->s; }

void l1_view_get(l1_t *l, l1_view *v)
{
    int lang = l->env.lang;
    memset(v, 0, sizeof *v);
    snprintf(v->title, sizeof v->title, "%s", l1_text(L1T_TITLE, lang));
    snprintf(v->subtitle, sizeof v->subtitle, "%s", l1_text(L1T_SUBTITLE, lang));
    v->nrows = build_rows(l, v->rows);
    if (l->sel < 0 || l->sel >= v->nrows) l->sel = 0;
    v->sel = l->sel;
    snprintf(v->hint, sizeof v->hint, "%s", l1_text(L1T_HINT, lang));
    snprintf(v->leg_ok, sizeof v->leg_ok, "%s", l1_text(L1T_LEG_OK, lang));
    snprintf(v->leg_back, sizeof v->leg_back, "%s", l1_text(L1T_LEG_BACK, lang));
    v->pads = l->env.pads_connected;
    l1_assign a;
    l1_assign_get(&l->s, &a);
    v->cards_on = l->s.armed;
    snprintf(v->card[0].who, sizeof v->card[0].who, "%s", l1_text(L1T_PLAYER1, lang));
    snprintf(v->card[1].who, sizeof v->card[1].who, "%s", l1_text(L1T_PLAYER2, lang));
    const char *lead = lead_name(l), *part = partner_name(l);
    snprintf(v->card[0].chr, sizeof v->card[0].chr, "%s", a.unit1 == 0 ? lead : part);
    snprintf(v->card[1].chr, sizeof v->card[1].chr, "%s", a.unit2 == 0 ? lead : part);
    snprintf(v->card[0].pad, sizeof v->card[0].pad, "%s", l1_text(a.swap ? L1T_PAD2 : L1T_PAD1, lang));
    snprintf(v->card[1].pad, sizeof v->card[1].pad, "%s", l1_text(a.swap ? L1T_PAD1 : L1T_PAD2, lang));
    if (l->s.armed) {
        if (l->env.pads_connected <= 0) {
            snprintf(v->status, sizeof v->status, "%s", l1_text(L1T_WARN_NOPAD, lang));
            v->status_kind = L1S_WARN;
        } else if (l->env.pads_connected == 1) {
            snprintf(v->status, sizeof v->status, "%s", l1_text(L1T_WARN_ONEPAD, lang));
            v->status_kind = L1S_WARN;
        } else {
            l1_expand(l1_text(L1T_ST_ON, lang), lang, v->status, sizeof v->status);
            v->status_kind = L1S_OK;
        }
        snprintf(v->note, sizeof v->note, "%s  %s", l1_text(L1T_NOTE_SAVE, lang), l1_text(L1T_NOTE_SOLO, lang));
    } else {
        snprintf(v->status, sizeof v->status, "%s", l1_text(L1T_ST_OFF, lang));
        v->status_kind = L1S_INFO;
    }
    if (!l->env.section_known && l->s.armed)
        snprintf(v->note2, sizeof v->note2, "%s  %s", l1_text(L1T_NEXT_SECTION, lang), l1_text(L1T_RAID_NOTE, lang));
    else
        snprintf(v->note2, sizeof v->note2, "%s", l1_text(L1T_RAID_NOTE, lang));
}

void l1_hint_text(const l1_settings *s, const l1_env *env, char *out, size_t size)
{
    int lang = env ? env->lang : 0;
    if (lang < 0 || lang >= L1_LANGS) lang = 0;
    if (!s->armed) {
        snprintf(out, size, "%s", l1_text(L1T_HINT_TITLE, lang));
        return;
    }
    l1_assign a;
    l1_assign_get(s, &a);
    const char *lead = env && env->lead[0] ? env->lead : l1_person_name(0, lang);
    const char *part = env && env->partner[0] ? env->partner : l1_person_name(2, lang);
    snprintf(out, size, "%s:  %s / %s    |    %s", l1_text(L1T_BANNER_ON, lang), a.unit1 == 0 ? lead : part, a.unit2 == 0 ? lead : part, l1_text(L1T_HINT_TITLE, lang));
}

/* ---------------------------------------------------------------- settings file */
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

int l1_ini_load(const char *path, l1_settings *s, int *lang_override)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "armed")) s->armed = atoi(v) != 0;
        else if (!strcmp(k, "role")) s->role = !strcmp(v, "partner") ? L1_PARTNER : L1_LEAD;
        else if (!strcmp(k, "layout")) s->layout = !strcmp(v, "top") ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE;
        else if (!strcmp(k, "pads")) s->pads_swapped = !strcmp(v, "swapped");
        else if (!strcmp(k, "language") && lang_override) {
            int i = l1_lang_from_code(v);
            *lang_override = i;   /* -1 = auto */
        }
    }
    fclose(f);
    sanitize(s);
    return 1;
}

int l1_ini_save(const char *path, const l1_settings *s, int lang_override)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    fprintf(f, "# Resident Evil Revelations co-op mod: settings (written by the Co-Op Play screen)\r\n");
    fprintf(f, "armed=%d\r\nrole=%s\r\nlayout=%s\r\npads=%s\r\n", s->armed ? 1 : 0, s->role == L1_PARTNER ? "partner" : "lead", s->layout == L1_LAYOUT_TOPBOTTOM ? "top" : "side",
            s->pads_swapped ? "swapped" : "normal");
    if (lang_override >= 0 && lang_override < L1_LANGS) fprintf(f, "language=%s\r\n", l1_lang_codes[lang_override]);
    if (fclose(f) != 0) return 0;
    remove(path);
    return rename(tmp, path) == 0;
}
