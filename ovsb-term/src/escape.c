/* ovsb.term — escape.c — parser VT100/xterm (moe moe kyun <3)
 *
 * Máquina de estados PS_* de term.h com UTF-8 integrado.
 * Implementa: C0, CSI completo, SGR completo, ESC simples,
 * OSC, DCS/SOS/PM/APC (descartados). Respostas via pty_write.
 */
#include "term.h"

#include <string.h>
#include <stdio.h>

/* bit marca param separado por ':' (subparâmetro, ex SGR 4:3) */
#define COLON_BIT (1 << 28)
#define CSI_MAX 9999

/* paleta ANSI 0-7 -> brilho normal */
static const uint8_t sgr_std_map[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

void parser_init(parser_t *p)
{
    p->state = PS_GROUND;
    p->nparams = 0;
    for (int i = 0; i < 16; i++) {
        p->params[i] = 0;
        p->param_has_value[i] = false;
    }
    p->current_param = -1; /* -1=vazio, -2=vazio pós-sep, >=0=valor */
    p->private_marker = 0;
    p->intermediate = 0;
    p->osc_len = 0;
    p->osc_esc = false;
    p->dcs_len = 0;
    p->utf8_cp = 0;
    p->utf8_remaining = 0;
}

/* zera só a parte CSI */
static void csi_reset(parser_t *p)
{
    p->nparams = 0;
    for (int i = 0; i < 16; i++) {
        p->params[i] = 0;
        p->param_has_value[i] = false;
    }
    p->current_param = -1;
    p->private_marker = 0;
    p->intermediate = 0;
}

/* empilha current_param (sep indica se o separador foi ':' p/ próximo) */
static void csi_push(parser_t *p, bool colon_sep)
{
    if (p->nparams >= 16) {
        p->current_param = -2;
        p->intermediate = colon_sep ? ':' : 0;
        return;
    }
    int v = (p->current_param >= 0) ? p->current_param : 0;
    bool has = (p->current_param >= 0);
    if (p->intermediate == ':')
        v |= COLON_BIT;
    p->params[p->nparams] = v;
    p->param_has_value[p->nparams] = has;
    p->nparams++;
    p->current_param = -2;
    p->intermediate = colon_sep ? ':' : 0;
}

/* dígito em construção */
static void csi_digit(parser_t *p, int d)
{
    if (p->current_param < 0)
        p->current_param = d;
    else if (p->current_param <= CSI_MAX)
        p->current_param = p->current_param * 10 + d;
}

/* fecha params no dispatch final */
static void csi_close(parser_t *p)
{
    if (p->current_param >= 0) {
        if (p->nparams < 16) {
            int v = p->current_param;
            if (p->intermediate == ':')
                v |= COLON_BIT;
            p->params[p->nparams] = v;
            p->param_has_value[p->nparams] = true;
            p->nparams++;
        }
    } else if (p->current_param == -2) {
        if (p->nparams < 16) {
            int v = 0;
            if (p->intermediate == ':')
                v |= COLON_BIT;
            p->params[p->nparams] = v;
            p->param_has_value[p->nparams] = false;
            p->nparams++;
        }
    } else if (p->nparams == 0) {
        p->params[0] = 0;
        p->param_has_value[0] = false;
        p->nparams = 1;
    }
    if (p->intermediate == ':')
        p->intermediate = 0;
}

static int csi_val(const parser_t *p, int i, int def)
{
    if (i < p->nparams && p->param_has_value[i])
        return p->params[i] & ~COLON_BIT;
    return def;
}

/* responde de volta pro pty */
static void reply(term_t *t, const char *s, size_t n)
{
    pty_write(t, s, n);
}

/* cor ARGB p/ OSC 10/11/12 e palette: #rgb, #rrggbb, rgb:r/g/b */
static uint32_t osc_parse_color(const char *s, uint32_t fallback)
{
    if (!s || !*s)
        return fallback;
    if (s[0] == '#') {
        size_t n = strlen(s + 1);
        unsigned int r = 0, g = 0, b = 0;
        if (n == 3) {
            unsigned int v[3] = { 0, 0, 0 };
            for (int i = 0; i < 3; i++) {
                char c = s[1 + i];
                unsigned int d;
                if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
                else return fallback;
                v[i] = d * 17;
            }
            r = v[0]; g = v[1]; b = v[2];
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        if (n == 6) {
            for (int i = 0; i < 3; i++) {
                unsigned int d = 0;
                for (int k = 0; k < 2; k++) {
                    char c = s[1 + i * 2 + k];
                    d <<= 4;
                    if (c >= '0' && c <= '9') d |= (unsigned int)(c - '0');
                    else if (c >= 'a' && c <= 'f') d |= (unsigned int)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') d |= (unsigned int)(c - 'A' + 10);
                    else return fallback;
                }
                if (i == 0) r = d; else if (i == 1) g = d; else b = d;
            }
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        return util_parse_color(s);
    }
    if (strncmp(s, "rgb:", 4) == 0) {
        /* rgb:rrrr/gggg/bbbb — pega byte alto de cada */
        unsigned int comp[3] = { 0, 0, 0 };
        const char *q = s + 4;
        for (int i = 0; i < 3; i++) {
            unsigned int v = 0;
            int digits = 0;
            while ((*q >= '0' && *q <= '9') ||
                   (*q >= 'a' && *q <= 'f') ||
                   (*q >= 'A' && *q <= 'F')) {
                unsigned int d;
                if (*q >= '0' && *q <= '9') d = (unsigned int)(*q - '0');
                else if (*q >= 'a' && *q <= 'f') d = (unsigned int)(*q - 'a' + 10);
                else d = (unsigned int)(*q - 'A' + 10);
                v = v * 16 + d;
                digits++;
                q++;
            }
            if (digits == 0)
                return fallback;
            comp[i] = (digits <= 2) ? (v << (8 - digits * 4)) : (v >> ((digits - 2) * 4));
            if (comp[i] > 255)
                comp[i] = 255;
            if (*q == '/')
                q++;
        }
        return 0xFF000000u | (comp[0] << 16) | (comp[1] << 8) | comp[2];
    }
    return util_parse_color(s);
}

/* base64 decode p/ OSC 52; retorna bytes ou -1 */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    int acc = 0, bits = 0;
    for (const char *q = in; *q; q++) {
        if (*q == '=')
            break;
        if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
            continue;
        int v = b64_val(*q);
        if (v < 0)
            return -1;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outsz)
                return -1;
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    return (int)o;
}

/* SGR completo */
static void sgr_dispatch(term_t *t, parser_t *p)
{
    if (p->nparams == 0) {
        grid_reset_attrs(t);
        return;
    }
    for (int i = 0; i < p->nparams; i++) {
        int raw = p->params[i];
        bool colon = (raw & COLON_BIT) != 0;
        int v = raw & ~COLON_BIT;
        bool has = p->param_has_value[i];
        if (!has)
            v = 0;
        /* estilo sublinhado 4:x (dois-pontos) — grid só tem on/off */
        if (colon && i > 0 && ((p->params[i - 1] & ~COLON_BIT) == 4) &&
            v >= 0 && v <= 5) {
            t->grid.cur_attrs |= ATTR_UNDERLINE;
            continue;
        }
        switch (v) {
        case 0: grid_reset_attrs(t); break;
        case 1: t->grid.cur_attrs |= ATTR_BOLD; break;
        case 2: t->grid.cur_attrs |= ATTR_DIM; break;
        case 3: t->grid.cur_attrs |= ATTR_ITALIC; break;
        case 4: t->grid.cur_attrs |= ATTR_UNDERLINE; break;
        case 5: case 6: t->grid.cur_attrs |= ATTR_BLINK; break;
        case 7: t->grid.cur_attrs |= ATTR_REVERSE; break;
        case 8: t->grid.cur_attrs |= ATTR_HIDDEN; break;
        case 9: t->grid.cur_attrs |= ATTR_STRIKE; break;
        case 21: t->grid.cur_attrs |= ATTR_UNDERLINE; break; /* duplo */
        case 22:
            t->grid.cur_attrs &= (uint16_t)~(ATTR_BOLD | ATTR_DIM);
            break;
        case 23: t->grid.cur_attrs &= (uint16_t)~ATTR_ITALIC; break;
        case 24: t->grid.cur_attrs &= (uint16_t)~ATTR_UNDERLINE; break;
        case 25: t->grid.cur_attrs &= (uint16_t)~ATTR_BLINK; break;
        case 27: t->grid.cur_attrs &= (uint16_t)~ATTR_REVERSE; break;
        case 28: t->grid.cur_attrs &= (uint16_t)~ATTR_HIDDEN; break;
        case 29: t->grid.cur_attrs &= (uint16_t)~ATTR_STRIKE; break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            grid_set_fg(t, t->grid.palette[sgr_std_map[v - 30]]);
            break;
        case 38: { /* fg estendido */
            if (i + 2 < p->nparams &&
                (p->params[i + 1] & ~COLON_BIT) == 5) {
                int idx = p->params[i + 2] & ~COLON_BIT;
                grid_set_fg(t, t->grid.palette[idx & 255]);
                i += 2;
            } else if (i + 4 < p->nparams &&
                       (p->params[i + 1] & ~COLON_BIT) == 2) {
                unsigned int r = (unsigned int)(p->params[i + 2] & ~COLON_BIT);
                unsigned int g = (unsigned int)(p->params[i + 3] & ~COLON_BIT);
                unsigned int b = (unsigned int)(p->params[i + 4] & ~COLON_BIT);
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                grid_set_fg(t, 0xFF000000u | (r << 16) | (g << 8) | b);
                i += 4;
            }
            break;
        }
        case 39: grid_set_fg(t, t->grid.default_fg); break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            grid_set_bg(t, t->grid.palette[sgr_std_map[v - 40]]);
            break;
        case 48: { /* bg estendido */
            if (i + 2 < p->nparams &&
                (p->params[i + 1] & ~COLON_BIT) == 5) {
                int idx = p->params[i + 2] & ~COLON_BIT;
                grid_set_bg(t, t->grid.palette[idx & 255]);
                i += 2;
            } else if (i + 4 < p->nparams &&
                       (p->params[i + 1] & ~COLON_BIT) == 2) {
                unsigned int r = (unsigned int)(p->params[i + 2] & ~COLON_BIT);
                unsigned int g = (unsigned int)(p->params[i + 3] & ~COLON_BIT);
                unsigned int b = (unsigned int)(p->params[i + 4] & ~COLON_BIT);
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                grid_set_bg(t, 0xFF000000u | (r << 16) | (g << 8) | b);
                i += 4;
            }
            break;
        }
        case 49: grid_set_bg(t, t->grid.default_bg); break;
        case 53: t->grid.cur_attrs |= ATTR_OVERLINE; break;
        case 55: t->grid.cur_attrs &= (uint16_t)~ATTR_OVERLINE; break;
        default:
            if (v >= 90 && v <= 97)
                grid_set_fg(t, t->grid.palette[(v - 90) + 8]);
            else if (v >= 100 && v <= 107)
                grid_set_bg(t, t->grid.palette[(v - 100) + 8]);
            break;
        }
    }
}

/* modos privados ?25 ?1049 ?2004 ?1000/1002/1006/1015 ?7 ?6 ?12 ?1 ?47 ?1047 ?1048 ?9 */
static void decset(term_t *t, parser_t *p, bool set)
{
    for (int i = 0; i < p->nparams; i++) {
        int v = p->params[i] & ~COLON_BIT;
        switch (v) {
        case 1: t->grid.app_cursor = set; break;
        case 6: /* DECOM */
            t->grid.origin_mode = set;
            if (set)
                grid_cursor_set(t, 0, t->grid.scroll_top);
            else
                grid_cursor_set(t, 0, 0);
            break;
        case 7: t->grid.auto_wrap = set; break;
        case 9: /* X10 antigo */
        case 1000: case 1002: case 1015: case 1016:
            t->grid.mouse_tracking = set;
            t->input.mouse_tracking = set;
            if (set && t->input.mouse_mode == 0)
                t->input.mouse_mode = 1; /* X10 */
            if (!set)
                t->input.mouse_mode = 0;
            break;
        case 1006: /* SGR mouse */
            t->grid.mouse_tracking = set;
            t->input.mouse_tracking = set;
            t->input.mouse_mode = set ? 2 : 0;
            break;
        case 12: t->grid.cursor_blink = set; break;
        case 25: t->grid.cursor_visible = set; break;
        case 47: case 1047: /* alt screen lite */
            if (set) {
                grid_cursor_save(t);
                grid_erase_display(t, 2);
            } else {
                grid_cursor_restore(t);
            }
            break;
        case 1048:
            if (set)
                grid_cursor_save(t);
            else
                grid_cursor_restore(t);
            break;
        case 1049: /* alt screen + save */
            if (set) {
                grid_cursor_save(t);
                grid_erase_display(t, 2);
            } else {
                grid_erase_display(t, 2);
                grid_cursor_restore(t);
            }
            break;
        case 2004: t->grid.bracketed_paste = set; break;
        default: break; /* 3,5,69,80,2026... ignora */
        }
    }
}

/* CSI dispatch */
static void csi_dispatch(term_t *t, parser_t *p, char final)
{
    csi_close(p);
    grid_t *g = &t->grid;

    /* DA2 / DA3 com privado '>' '=' */
    if (p->private_marker == '>' && final == 'c') {
        static const char da2[] = "\x1b[>1;10;0c";
        reply(t, da2, sizeof(da2) - 1);
        return;
    }
    if (p->private_marker == '=' && final == 'c')
        return; /* tertiary: sem resposta */

    if (p->private_marker == '?') {
        switch (final) {
        case 'h': decset(t, p, true); return;
        case 'l': decset(t, p, false); return;
        case 'J': grid_erase_display(t, csi_val(p, 0, 0)); return;
        case 'K': grid_erase_line(t, csi_val(p, 0, 0)); return;
        case 'c': /* DA privado: ignora */ return;
        case 'n': return; /* DSR privado: ignora */
        default: return;
        }
    }
    if (p->private_marker != 0 && p->private_marker != '?' &&
        p->private_marker != '>' && p->private_marker != '=') {
        if (final != 'c' || p->private_marker != '<')
            return;
    }

    switch (final) {
    case 'A': grid_cursor_move(t, 0, -csi_val(p, 0, 1)); break;
    case 'B': grid_cursor_move(t, 0, csi_val(p, 0, 1)); break;
    case 'C': grid_cursor_move(t, csi_val(p, 0, 1), 0); break;
    case 'D': grid_cursor_move(t, -csi_val(p, 0, 1), 0); break;
    case 'E': /* CNL */
        grid_cursor_set(t, 0, g->cursor_y + csi_val(p, 0, 1));
        break;
    case 'F': /* CPL */
        grid_cursor_set(t, 0, g->cursor_y - csi_val(p, 0, 1));
        break;
    case 'G': /* CHA */
        grid_cursor_set(t, csi_val(p, 0, 1) - 1, g->cursor_y);
        break;
    case 'H': case 'f': { /* CUP / HVP */
        int r = csi_val(p, 0, 1) - 1;
        int c = csi_val(p, 1, 1) - 1;
        if (g->origin_mode)
            r += g->scroll_top;
        if (r < 0) r = 0;
        if (c < 0) c = 0;
        grid_cursor_set(t, c, r);
        break;
    }
    case 'd': /* VPA */
        grid_cursor_set(t, g->cursor_x, csi_val(p, 0, 1) - 1);
        break;
    case 'e': /* VPR */
        grid_cursor_move(t, 0, csi_val(p, 0, 1));
        break;
    case 'J': { /* ED */
        int m = csi_val(p, 0, 0);
        if (m == 3) {
            scrollback_clear(t);
            grid_erase_display(t, 2);
        } else {
            grid_erase_display(t, m);
        }
        break;
    }
    case 'K': grid_erase_line(t, csi_val(p, 0, 0)); break;
    case 'L': grid_insert_lines(t, csi_val(p, 0, 1)); break;
    case 'M': grid_delete_lines(t, csi_val(p, 0, 1)); break;
    case 'P': grid_delete_chars(t, csi_val(p, 0, 1)); break;
    case '@': grid_insert_chars(t, csi_val(p, 0, 1)); break;
    case 'S': grid_scroll_up(t, csi_val(p, 0, 1)); break;
    case 'T': grid_scroll_down(t, csi_val(p, 0, 1)); break;
    case 'X': grid_erase_chars(t, csi_val(p, 0, 1)); break;
    case 'Z': { /* CBT */
        int n = csi_val(p, 0, 1);
        int x = g->cursor_x;
        for (int k = 0; k < n; k++) {
            x = ((x - 1) / 8) * 8;
            if (x < 0) { x = 0; break; }
        }
        grid_cursor_set(t, x, g->cursor_y);
        break;
    }
    case 'b': { /* REP */
        int n = csi_val(p, 0, 1);
        if (n < 1) n = 1;
        if (n > 1000) n = 1000;
        int w = util_wcwidth(g->last_char);
        if (w < 1) w = 1;
        for (int k = 0; k < n; k++)
            grid_put_char(t, g->last_char, w);
        break;
    }
    case 'c': { /* DA primário */
        static const char da[] = "\x1b[?1;2c";
        reply(t, da, sizeof(da) - 1);
        break;
    }
    case 'g': break; /* TBC: sem tab stops — ignora */
    case 'h': /* SM */
        for (int i = 0; i < p->nparams; i++) {
            int v = p->params[i] & ~COLON_BIT;
            if (v == 4)
                g->insert_mode = true;
        }
        break;
    case 'l': /* RM */
        for (int i = 0; i < p->nparams; i++) {
            int v = p->params[i] & ~COLON_BIT;
            if (v == 4)
                g->insert_mode = false;
        }
        break;
    case 'm': sgr_dispatch(t, p); break;
    case 'n': { /* DSR */
        int v = csi_val(p, 0, 0);
        if (v == 5) {
            static const char ok[] = "\x1b[0n";
            reply(t, ok, sizeof(ok) - 1);
        } else if (v == 6) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                             g->cursor_y + 1, g->cursor_x + 1);
            if (n > 0)
                reply(t, buf, (size_t)n);
        }
        break;
    }
    case 'p': /* '!' + 'p' = DECSTR soft reset */
        if (p->intermediate == '!') {
            grid_reset_attrs(t);
            g->origin_mode = false;
            g->insert_mode = false;
        }
        break;
    case 'q': /* DECSCUSR (com ' ' intermediário) */
        if (p->intermediate == ' ' || p->intermediate == 0) {
            int v = csi_val(p, 0, 0);
            switch (v) {
            case 0: case 1:
                g->cursor_style = CURSOR_BLOCK; g->cursor_blink = true; break;
            case 2: g->cursor_style = CURSOR_BLOCK; g->cursor_blink = false; break;
            case 3: g->cursor_style = CURSOR_UNDERLINE; g->cursor_blink = true; break;
            case 4: g->cursor_style = CURSOR_UNDERLINE; g->cursor_blink = false; break;
            case 5: g->cursor_style = CURSOR_BAR; g->cursor_blink = true; break;
            case 6: g->cursor_style = CURSOR_BAR; g->cursor_blink = false; break;
            default: break;
            }
        }
        break;
    case 'r': { /* DECSTBM */
        int rows = g->rows > 0 ? g->rows : 24;
        int top = (p->nparams >= 1 && p->param_has_value[0])
                      ? (p->params[0] & ~COLON_BIT) - 1 : 0;
        int bot = (p->nparams >= 2 && p->param_has_value[1])
                      ? (p->params[1] & ~COLON_BIT) - 1 : rows - 1;
        if (top < 0) top = 0;
        if (bot >= rows) bot = rows - 1;
        if (top >= bot) { top = 0; bot = rows - 1; }
        g->scroll_top = top;
        g->scroll_bottom = bot;
        grid_cursor_set(t, 0, top);
        break;
    }
    case 's': grid_cursor_save(t); break;    /* SCOSC */
    case 'u': grid_cursor_restore(t); break; /* SCORC */
    case 't': { /* WindowOps */
        int v = csi_val(p, 0, 0);
        if (v == 20 || v == 21) {
            char pre[4];
            pre[0] = '\x1b'; pre[1] = ']';
            pre[2] = (v == 21) ? 'l' : 'L';
            pre[3] = '\0';
            reply(t, pre, 3);
            reply(t, t->title, strlen(t->title));
            static const char st[] = "\x1b\\";
            reply(t, st, sizeof(st) - 1);
        }
        /* 8/14/18 etc: ignora */
        break;
    }
    default: break; /* finais desconhecidos: ignora */
    }
}

/* OSC dispatch — buf já sem terminador */
static void osc_dispatch(term_t *t, parser_t *p)
{
    p->osc_buf[p->osc_len] = '\0';
    char *semi = strchr(p->osc_buf, ';');
    int ps = -1;
    const char *pt = "";
    if (semi) {
        /* Ps pode ser vazio? trata como -1 */
        char save = *semi;
        *semi = '\0';
        if (p->osc_buf[0] != '\0') {
            int v = 0;
            for (const char *q = p->osc_buf; *q; q++) {
                if (*q < '0' || *q > '9') { v = -1; break; }
                v = v * 10 + (*q - '0');
            }
            ps = v;
        }
        *semi = save;
        pt = semi + 1;
    } else if (p->osc_buf[0] >= '0' && p->osc_buf[0] <= '9') {
        int v = 0;
        for (const char *q = p->osc_buf; *q; q++) {
            if (*q < '0' || *q > '9') { v = -1; break; }
            v = v * 10 + (*q - '0');
        }
        ps = v;
    }
    switch (ps) {
    case 0: case 1: case 2: /* título */
        snprintf(t->title, sizeof(t->title), "%s", pt);
        break;
    case 4: { /* palette: idx;cor[;idx;cor...] */
        const char *q = pt;
        while (*q) {
            int idx = 0, digits = 0;
            while (*q >= '0' && *q <= '9') {
                idx = idx * 10 + (*q - '0');
                digits++;
                q++;
            }
            if (digits == 0 || *q != ';')
                break;
            q++;
            const char *ce = strchr(q, ';');
            char color[64];
            if (ce) {
                size_t n = (size_t)(ce - q);
                if (n >= sizeof(color))
                    n = sizeof(color) - 1;
                memcpy(color, q, n);
                color[n] = '\0';
                q = ce + 1;
            } else {
                snprintf(color, sizeof(color), "%s", q);
                q += strlen(q);
            }
            if (idx >= 0 && idx < 256)
                t->grid.palette[idx] = osc_parse_color(color,
                    t->grid.palette[idx]);
        }
        break;
    }
    case 7: case 9: case 133: break; /* cwd/notify/shell-int: ignora */
    case 8: break; /* hyperlink: ignora */
    case 10:
        t->grid.default_fg = osc_parse_color(pt, t->grid.default_fg);
        break;
    case 11:
        t->grid.default_bg = osc_parse_color(pt, t->grid.default_bg);
        break;
    case 12:
        t->grid.cursor_color = osc_parse_color(pt, t->grid.cursor_color);
        break;
    case 52: { /* clipboard: c;base64 */
        if (pt[0] == '\0' || pt[1] != ';')
            break;
        const char *b64 = pt + 2;
        if (b64[0] == '?')
            break; /* query: sem resposta */
        static char decoded[4096];
        int n = base64_decode(b64, decoded, sizeof(decoded));
        if (n >= 0)
            clipboard_set(t, decoded, (size_t)n);
        break;
    }
    case 104: { /* reset palette */
        if (pt[0] == '\0') {
            for (int i = 0; i < 16; i++)
                t->grid.palette[i] = t->config.palette[i];
        } else {
            const char *q = pt;
            while (*q) {
                int idx = 0, digits = 0;
                while (*q >= '0' && *q <= '9') {
                    idx = idx * 10 + (*q - '0');
                    digits++;
                    q++;
                }
                if (digits == 0)
                    break;
                if (idx >= 0 && idx < 16)
                    t->grid.palette[idx] = t->config.palette[idx];
                if (*q == ';')
                    q++;
                else
                    break;
            }
        }
        break;
    }
    case 110: t->grid.default_fg = t->config.fg; break;
    case 111: t->grid.default_bg = t->config.bg; break;
    case 112: t->grid.cursor_color = t->config.cursor; break;
    default: break;
    }
}

/* ESC simples (fora de CSI/OSC/DCS) */
static void esc_dispatch(term_t *t, char c)
{
    grid_t *g = &t->grid;
    switch (c) {
    case '7': grid_cursor_save(t); break;    /* DECSC */
    case '8': grid_cursor_restore(t); break; /* DECRC */
    case 'D': grid_linefeed(t); break;       /* IND */
    case 'E': /* NEL */
        grid_carriage_return(t);
        grid_linefeed(t);
        break;
    case 'H': break; /* HTS: sem tab stops — ignora */
    case 'M': /* RI */
        if (g->cursor_y <= g->scroll_top)
            grid_scroll_down(t, 1);
        else
            grid_cursor_move(t, 0, -1);
        break;
    case 'c': /* RIS full reset */
        grid_reset_attrs(t);
        g->origin_mode = false;
        g->insert_mode = false;
        g->auto_wrap = true;
        g->wrap_next = false;
        g->scroll_top = 0;
        g->scroll_bottom = g->rows > 0 ? g->rows - 1 : 23;
        g->bracketed_paste = false;
        g->mouse_tracking = false;
        g->app_cursor = false;
        g->app_keypad = false;
        g->cursor_visible = true;
        grid_erase_display(t, 2);
        grid_cursor_set(t, 0, 0);
        break;
    case '=': g->app_keypad = true; break;  /* DECKPAM */
    case '>': g->app_keypad = false; break; /* DECKPNM */
    case 'N': case 'O': break; /* SS2/SS3: ignora */
    default: break;
    }
}

/* emite codepoint p/ o grid */
static void emit_cp(term_t *t, parser_t *p, uint32_t cp)
{
    (void)p;
    int w = util_wcwidth(cp);
    if (w < 1)
        w = 1;
    if (w > 2)
        w = 2;
    grid_put_char(t, cp, w);
}

static void emit_replacement(term_t *t, parser_t *p)
{
    emit_cp(t, p, 0xFFFD);
}

void escape_feed(term_t *t, const char *buf, size_t len)
{
    parser_t *p = &t->parser;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];

        /* C1 de byte único vindos do GROUND */
        if (p->state == PS_GROUND && p->utf8_remaining == 0 &&
            c >= 0x80 && c <= 0x9F) {
            switch (c) {
            case 0x84: grid_linefeed(t); continue;         /* IND */
            case 0x85:                                    /* NEL */
                grid_carriage_return(t);
                grid_linefeed(t);
                continue;
            case 0x88: continue;                          /* HTS ignora */
            case 0x8D: esc_dispatch(t, 'M'); continue;     /* RI */
            case 0x90: p->dcs_len = 0; p->intermediate = 0;
                p->state = PS_DCS_ENTRY; continue;
            case 0x98: case 0x9E: case 0x9F:
                p->intermediate = 0;
                p->state = PS_SOS_PM_APC; continue;
            case 0x9B: csi_reset(p); p->state = PS_CSI_ENTRY; continue;
            case 0x9C: p->state = PS_GROUND; continue;    /* ST solto */
            case 0x9D: p->osc_len = 0; p->osc_esc = false;
                p->state = PS_OSC_STRING; continue;
            default: continue; /* outros C1: ignora */
            }
        }

        switch (p->state) {
        case PS_GROUND:
            if (c < 0x80) {
                p->utf8_remaining = 0;
                p->utf8_cp = 0;
                switch (c) {
                case 0x07: break; /* BEL */
                case 0x08: grid_backspace(t); break;
                case 0x09: grid_tab(t); break;
                case 0x0A: case 0x0B: case 0x0C:
                    grid_linefeed(t); break;
                case 0x0D: grid_carriage_return(t); break;
                case 0x1B:
                    p->state = PS_ESC;
                    break;
                case 0x7F: break; /* DEL ignora */
                default:
                    if (c >= 0x20)
                        emit_cp(t, p, c);
                    /* outros C0: ignora */
                    break;
                }
            } else if (p->utf8_remaining > 0) {
                if (c >= 0x80 && c <= 0xBF) {
                    p->utf8_cp = (p->utf8_cp << 6) | (c & 0x3F);
                    p->utf8_remaining--;
                    if (p->utf8_remaining == 0) {
                        uint32_t cp = p->utf8_cp;
                        /* valida: substitui inválidos */
                        if ((cp >= 0xD800 && cp <= 0xDFFF) ||
                            cp > 0x10FFFF || cp < 0x80)
                            emit_replacement(t, p);
                        else
                            emit_cp(t, p, cp);
                        p->utf8_cp = 0;
                    }
                } else {
                    /* sequência quebrada */
                    p->utf8_remaining = 0;
                    p->utf8_cp = 0;
                    i--; /* reprocessa byte atual */
                }
            } else {
                if (c >= 0xC2 && c <= 0xDF) {
                    p->utf8_cp = c & 0x1F;
                    p->utf8_remaining = 1;
                } else if (c >= 0xE0 && c <= 0xEF) {
                    p->utf8_cp = c & 0x0F;
                    p->utf8_remaining = 2;
                } else if (c >= 0xF0 && c <= 0xF4) {
                    p->utf8_cp = c & 0x07;
                    p->utf8_remaining = 3;
                } else {
                    /* 0x80-0xBF solto, 0xC0/0xC1, 0xF5-0xFF */
                    emit_replacement(t, p);
                }
            }
            break;

        case PS_ESC:
            if (c == 0x1B) {
                /* ESC ESC: fica em ESC */
                break;
            } else if (c == '[') {
                csi_reset(p);
                p->state = PS_CSI_ENTRY;
            } else if (c == ']') {
                p->osc_len = 0;
                p->osc_esc = false;
                p->state = PS_OSC_STRING;
            } else if (c == 'P') {
                p->dcs_len = 0;
                p->intermediate = 0;
                p->state = PS_DCS_ENTRY;
            } else if (c == 'X' || c == '^' || c == '_') {
                p->intermediate = 0;
                p->state = PS_SOS_PM_APC;
            } else if (c == '\\') {
                p->state = PS_GROUND; /* ST solto */
            } else if (c == '(' || c == ')' || c == '*' || c == '+' ||
                       c == '#' || c == '%' || c == ' ') {
                p->state = PS_ESC_INT; /* charset: consome 1 byte */
            } else if (c < 0x20 || c == 0x7F) {
                /* C0 dentro de ESC: ignora, mantém estado */
                break;
            } else {
                esc_dispatch(t, (char)c);
                p->state = PS_GROUND;
            }
            break;

        case PS_ESC_INT:
            /* consome o byte do charset e volta */
            p->state = PS_GROUND;
            break;

        case PS_CSI_ENTRY:
            if (c >= '0' && c <= '9') {
                csi_digit(p, c - '0');
                p->state = PS_CSI_PARAM;
            } else if (c == ';') {
                csi_push(p, false);
                p->state = PS_CSI_PARAM;
            } else if (c == ':') {
                csi_push(p, true);
                p->state = PS_CSI_PARAM;
            } else if (c == '?' || c == '>' || c == '=' || c == '<') {
                p->private_marker = (char)c;
            } else if (c >= 0x20 && c <= 0x2F) {
                p->intermediate = (char)c;
                p->state = PS_CSI_INT;
            } else if (c >= 0x40 && c <= 0x7E) {
                csi_dispatch(t, p, (char)c);
                p->state = PS_GROUND;
            } else if (c == 0x1B) {
                p->state = PS_ESC;
            } else if (c == 0x18 || c == 0x1A) {
                p->state = PS_GROUND; /* CAN/SUB aborta */
            } else {
                p->state = PS_CSI_IGNORE; /* resto: ignora */
            }
            break;

        case PS_CSI_PARAM:
            if (c >= '0' && c <= '9') {
                csi_digit(p, c - '0');
            } else if (c == ';') {
                csi_push(p, false);
            } else if (c == ':') {
                csi_push(p, true);
            } else if (c >= 0x20 && c <= 0x2F) {
                /* empilha pendente antes do intermediário */
                if (p->current_param >= 0 || p->current_param == -2) {
                    if (p->nparams < 16) {
                        int v = (p->current_param >= 0)
                                    ? p->current_param : 0;
                        bool has = (p->current_param >= 0);
                        if (p->intermediate == ':')
                            v |= COLON_BIT;
                        p->params[p->nparams] = v;
                        p->param_has_value[p->nparams] = has;
                        p->nparams++;
                    }
                    p->current_param = -1;
                }
                p->intermediate = (char)c;
                p->state = PS_CSI_INT;
            } else if (c >= 0x40 && c <= 0x7E) {
                csi_dispatch(t, p, (char)c);
                p->state = PS_GROUND;
            } else if (c == 0x1B) {
                p->state = PS_ESC;
            } else if (c == 0x18 || c == 0x1A) {
                p->state = PS_GROUND;
            } else if (c == '?' || c == '>' || c == '=' || c == '<') {
                p->state = PS_CSI_IGNORE;
            } else {
                /* C0: ignora */
            }
            break;

        case PS_CSI_INT:
            if (c >= 0x20 && c <= 0x2F) {
                p->intermediate = (char)c;
            } else if (c >= 0x40 && c <= 0x7E) {
                csi_dispatch(t, p, (char)c);
                p->state = PS_GROUND;
            } else if (c == 0x1B) {
                p->state = PS_ESC;
            } else if (c == 0x18 || c == 0x1A) {
                p->state = PS_GROUND;
            } else {
                p->state = PS_CSI_IGNORE;
            }
            break;

        case PS_CSI_IGNORE:
            if (c >= 0x40 && c <= 0x7E)
                p->state = PS_GROUND;
            else if (c == 0x1B)
                p->state = PS_ESC;
            else if (c == 0x18 || c == 0x1A)
                p->state = PS_GROUND;
            break;

        case PS_OSC_STRING:
            if (p->osc_esc) {
                p->osc_esc = false;
                if (c == '\\') {
                    osc_dispatch(t, p);
                    p->state = PS_GROUND;
                }
                /* outro byte após ESC: continua acumulando */
            } else if (c == 0x1B) {
                p->osc_esc = true;
            } else if (c == 0x07) { /* BEL termina */
                osc_dispatch(t, p);
                p->state = PS_GROUND;
            } else if (c == 0x9C) { /* ST C1 */
                osc_dispatch(t, p);
                p->state = PS_GROUND;
            } else if (c == 0x18 || c == 0x1A) {
                p->state = PS_GROUND; /* CAN/SUB aborta */
            } else if (c < 0x20) {
                /* outros C0: ignora */
            } else {
                if (p->osc_len < (int)sizeof(p->osc_buf) - 1)
                    p->osc_buf[p->osc_len++] = (char)c;
                /* overflow: consome e descarta o resto */
            }
            break;

        case PS_DCS_ENTRY:
            if (c == 0x1B) {
                p->intermediate = 0x1B;
            } else if (p->intermediate == 0x1B) {
                if (c == '\\')
                    p->state = PS_GROUND; /* ST vazio */
                else
                    p->state = PS_DCS_IGNORE;
                p->intermediate = 0;
            } else if (c == 0x9C) {
                p->state = PS_GROUND;
            } else if (c == 0x18 || c == 0x1A || c == 0x07) {
                p->state = PS_GROUND;
            } else if (c >= 0x40 && c <= 0x7E) {
                /* final DCS → passthru (descarta tudo: sixel/kitty) */
                p->state = PS_DCS_PASSTHRU;
                p->intermediate = 0;
            } else {
                /* params/intermediários: acumula p/ limite e ignora */
                if (p->dcs_len < (int)sizeof(p->dcs_buf) - 1)
                    p->dcs_buf[p->dcs_len++] = (char)c;
            }
            break;

        case PS_DCS_PASSTHRU:
            if (c == 0x1B) {
                p->intermediate = 0x1B;
            } else if (p->intermediate == 0x1B) {
                p->intermediate = 0;
                if (c == '\\')
                    p->state = PS_GROUND; /* ST: fim */
                /* senão continua descartando */
            } else if (c == 0x9C) {
                p->state = PS_GROUND;
            } else if (c == 0x18 || c == 0x1A) {
                p->state = PS_GROUND;
            } else {
                /* descarta (sixel/kitty/etc) */
            }
            break;

        case PS_DCS_IGNORE:
            if (c == 0x1B) {
                p->intermediate = 0x1B;
            } else if (p->intermediate == 0x1B) {
                p->intermediate = 0;
                if (c == '\\')
                    p->state = PS_GROUND;
            } else if (c == 0x9C) {
                p->state = PS_GROUND;
            } else if (c == 0x18 || c == 0x1A) {
                p->state = PS_GROUND;
            }
            break;

        case PS_SOS_PM_APC:
            if (c == 0x1B) {
                p->intermediate = 0x1B;
            } else if (p->intermediate == 0x1B) {
                p->intermediate = 0;
                if (c == '\\')
                    p->state = PS_GROUND; /* ST: fim, descarta */
            } else if (c == 0x9C) {
                p->state = PS_GROUND;
            } else if (c == 0x18 || c == 0x1A || c == 0x07) {
                p->state = PS_GROUND;
            } else {
                /* consome e descarta */
            }
            break;
        }
    }
}
