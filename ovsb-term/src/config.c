/* ovsb.term — config.c — defaults + parser INI */
#include "term.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* paleta base 16 (xterm-ish do projeto) */
static const uint32_t base16[16] = {
    0xFF000000, 0xFFCD3131, 0xFF0DBC79, 0xFFE5E510,
    0xFF2472C8, 0xFFBC3FBC, 0xFF11A8CD, 0xFFE5E5E5,
    0xFF666666, 0xFFF14C4C, 0xFF23D18B, 0xFFF5F543,
    0xFF3B8EEA, 0xFFD670D6, 0xFF29B8DB, 0xFFE5E5E5,
};

/* copia string com NUL */
static void set_str(char *dst, size_t dstsz, const char *src)
{
    size_t n;
    if (dst == NULL || dstsz == 0)
        return;
    if (src == NULL)
        src = "";
    n = strlen(src);
    if (n > dstsz - 1)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* igual case-insensitive */
static int eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int parse_bool(const char *s, bool dflt)
{
    if (s == NULL)
        return dflt ? 1 : 0;
    if (eq_ci(s, "true") || eq_ci(s, "yes") || eq_ci(s, "on") ||
        eq_ci(s, "1"))
        return 1;
    if (eq_ci(s, "false") || eq_ci(s, "no") || eq_ci(s, "off") ||
        eq_ci(s, "0"))
        return 0;
    return dflt ? 1 : 0;
}

/* tira espaços das pontas (in place, retorna início) */
static char *trim(char *s)
{
    size_t len;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n'))
        len--;
    s[len] = '\0';
    return s;
}

void config_defaults(config_t *c)
{
    int i;
    if (c == NULL)
        return;
    set_str(c->font_family, sizeof(c->font_family), "monospace");
    c->font_size = 12;
    c->dpi = 96.0f;
    c->bg = 0xFF1E1E1E; /* #1e1e1e */
    c->fg = 0xFFD4D4D4; /* #d4d4d4 */
    c->cursor = 0xFFFFFFFF;
    c->sel_bg = 0xFF264F78;
    c->sel_fg = 0xFFFFFFFF;
    for (i = 0; i < 16; i++)
        c->palette[i] = base16[i];
    c->cursor_style = CURSOR_BLOCK;
    c->cursor_blink = true;
    c->cursor_blink_interval = 500;
    c->scrollback_lines = 10000;
    c->alt_sends_escape = true;
    c->shift_pgup_scroll = true;
    c->mouse_tracking = true;
    set_str(c->shell, sizeof(c->shell), "/bin/sh");
    set_str(c->term_name, sizeof(c->term_name), "ovsb.term");
}

/* aplica uma linha já separada em section/key=value */
static void apply_kv(config_t *c, const char *section,
                     const char *key, const char *val)
{
    int i;
    if (eq_ci(section, "font")) {
        if (eq_ci(key, "family"))
            set_str(c->font_family, sizeof(c->font_family), val);
        else if (eq_ci(key, "size"))
            c->font_size = atoi(val);
        else if (eq_ci(key, "dpi"))
            c->dpi = atoi(val);
    } else if (eq_ci(section, "colors")) {
        if (eq_ci(key, "background") || eq_ci(key, "bg"))
            c->bg = util_parse_color(val);
        else if (eq_ci(key, "foreground") || eq_ci(key, "fg"))
            c->fg = util_parse_color(val);
        else if (eq_ci(key, "cursor"))
            c->cursor = util_parse_color(val);
        else if (eq_ci(key, "selection_bg") || eq_ci(key, "sel_bg"))
            c->sel_bg = util_parse_color(val);
        else if (eq_ci(key, "selection_fg") || eq_ci(key, "sel_fg"))
            c->sel_fg = util_parse_color(val);
        else if (strncmp(key, "palette_", 8) == 0) {
            i = atoi(key + 8);
            if (i >= 0 && i < 16)
                c->palette[i] = util_parse_color(val);
        }
    } else if (eq_ci(section, "cursor")) {
        if (eq_ci(key, "style")) {
            if (eq_ci(val, "block"))
                c->cursor_style = CURSOR_BLOCK;
            else if (eq_ci(val, "underline"))
                c->cursor_style = CURSOR_UNDERLINE;
            else if (eq_ci(val, "bar"))
                c->cursor_style = CURSOR_BAR;
            else if (eq_ci(val, "hollow") || eq_ci(val, "block_hollow"))
                c->cursor_style = CURSOR_HOLLOW;
        } else if (eq_ci(key, "blink")) {
            c->cursor_blink = parse_bool(val, true) ? true : false;
        } else if (eq_ci(key, "blink_interval")) {
            c->cursor_blink_interval = atoi(val);
        } else if (eq_ci(key, "color")) {
            c->cursor = util_parse_color(val);
        }
    } else if (eq_ci(section, "scrollback")) {
        if (eq_ci(key, "lines"))
            c->scrollback_lines = atoi(val);
    } else if (eq_ci(section, "input")) {
        if (eq_ci(key, "alt_sends_escape"))
            c->alt_sends_escape = parse_bool(val, true) ? true : false;
        else if (eq_ci(key, "shift_pgup_scroll"))
            c->shift_pgup_scroll = parse_bool(val, true) ? true : false;
        else if (eq_ci(key, "mouse_tracking"))
            c->mouse_tracking = parse_bool(val, true) ? true : false;
    } else if (eq_ci(section, "main")) {
        if (eq_ci(key, "shell"))
            set_str(c->shell, sizeof(c->shell), val);
        else if (eq_ci(key, "term"))
            set_str(c->term_name, sizeof(c->term_name), val);
    }
}

void config_load(config_t *c, const char *path)
{
    FILE *f;
    char line[512];
    char section[32];

    if (c == NULL)
        return;
    config_defaults(c);
    if (path == NULL || *path == '\0')
        return;
    f = fopen(path, "r");
    if (f == NULL)
        return;
    section[0] = '\0';
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = trim(line);
        char *eq, *key, *val, *end;
        size_t j;

        if (*s == '\0' || *s == ';' || *s == '#')
            continue;
        if (*s == '[') {
            end = strchr(s, ']');
            if (end == NULL)
                continue;
            *end = '\0';
            s = trim(s + 1);
            for (j = 0; j < sizeof(section) - 1 && s[j]; j++) {
                char ch = s[j];
                if (ch >= 'A' && ch <= 'Z')
                    ch = (char)(ch + 32);
                section[j] = ch;
            }
            section[j] = '\0';
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        /* comentário inline ';' (valores nunca têm ';') */
        end = strchr(val, ';');
        if (end != NULL) {
            *end = '\0';
            val = trim(val);
        }
        /* '#' inline só se precedido de espaço e não for cor inicial */
        for (j = 0; val[j]; j++) {
            if (val[j] == '#' && j > 0 &&
                (val[j - 1] == ' ' || val[j - 1] == '\t')) {
                val[j] = '\0';
                val = trim(val);
                break;
            }
        }
        if (*key == '\0')
            continue;
        apply_kv(c, section, key, val);
    }
    fclose(f);
}

void config_reload(term_t *t)
{
    if (t == NULL)
        return;
    config_load(&t->config, t->config_path);
}
