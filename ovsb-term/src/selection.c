/* ovsb.term — selection.c — extrai texto do grid (moe moe kyun <3) */
#include "term.h"
#include "selection.h"

#include <stdlib.h>
#include <string.h>

/* encode local (sem depender de util.c ainda) */
static size_t sel_utf8_encode(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    out[0] = '?';
    return 1;
}

static void sel_normalize(const selection_t *s, int *x0, int *y0,
                          int *x1, int *y1)
{
    *x0 = s->start_x;
    *y0 = s->start_y;
    *x1 = s->end_x;
    *y1 = s->end_y;
    if (*y0 > *y1 || (*y0 == *y1 && *x0 > *x1)) {
        int tx = *x0, ty = *y0;
        *x0 = *x1;
        *y0 = *y1;
        *x1 = tx;
        *y1 = ty;
    }
}

/* extrai; caller free. NULL se sem seleção/grid */
static char *sel_extract(term_t *t, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!t)
        return NULL;
    grid_t *g = &t->grid;
    selection_t *s = &t->selection;
    if (!g->cells || g->rows <= 0 || g->cols <= 0)
        return NULL;

    int x0, y0, x1, y1;
    sel_normalize(s, &x0, &y0, &x1, &y1);

    /* clamp */
    if (y0 < 0) y0 = 0;
    if (y1 < 0) y1 = 0;
    if (y0 >= g->rows) y0 = g->rows - 1;
    if (y1 >= g->rows) y1 = g->rows - 1;
    if (x0 < 0) x0 = 0;
    if (x1 < 0) x1 = 0;
    if (x0 >= g->cols) x0 = g->cols - 1;
    if (x1 >= g->cols) x1 = g->cols - 1;

    int rx0 = x0 < x1 ? x0 : x1;
    int rx1 = x0 < x1 ? x1 : x0;
    int ry0 = y0 < y1 ? y0 : y1;
    int ry1 = y0 < y1 ? y1 : y0;

    size_t cap = 256;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;

    for (int y = (s->rectangular ? ry0 : y0); y <= (s->rectangular ? ry1 : y1); y++) {
        int xs, xe;
        if (s->rectangular) {
            xs = rx0;
            xe = rx1;
        } else if (y0 == y1) {
            xs = x0;
            xe = x1;
        } else if (y == y0) {
            xs = x0;
            xe = g->cols - 1;
        } else if (y == y1) {
            xs = 0;
            xe = x1;
        } else {
            xs = 0;
            xe = g->cols - 1;
        }
        if (xs < 0) xs = 0;
        if (xe >= g->cols) xe = g->cols - 1;

        /* junta a linha, depois trim trailing spaces */
        char line[4096 * 4];
        size_t llen = 0;
        for (int x = xs; x <= xe; x++) {
            cell_t *c = &g->cells[(size_t)y * (size_t)g->cols + (size_t)x];
            if (c->attrs & ATTR_WIDE_TAIL)
                continue; /* pula 2ª metade de CJK */
            uint32_t cp = c->codepoint;
            if (cp == 0)
                cp = ' ';
            char tmp[4];
            size_t n = sel_utf8_encode(tmp, cp);
            if (llen + n < sizeof(line)) {
                memcpy(line + llen, tmp, n);
                llen += n;
            }
        }
        /* trim espaços/tab do fim */
        while (llen > 0 && (line[llen - 1] == ' ' || line[llen - 1] == '\t'))
            llen--;

        size_t need = len + llen + 2;
        if (need > cap) {
            size_t ncap = cap * 2;
            while (ncap < need)
                ncap *= 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, line, llen);
        len += llen;
        /* '\n' entre linhas, nunca após a última */
        int last = s->rectangular ? ry1 : y1;
        if (y != last)
            buf[len++] = '\n';
    }

    if (len + 1 > cap) {
        char *nb = realloc(buf, len + 1);
        if (!nb) {
            free(buf);
            return NULL;
        }
        buf = nb;
    }
    buf[len] = '\0';
    if (out_len)
        *out_len = len;
    return buf;
}

void selection_start(term_t *t, int x, int y)
{
    if (!t)
        return;
    t->selection.start_x = x;
    t->selection.start_y = y;
    t->selection.end_x = x;
    t->selection.end_y = y;
    t->selection.active = true;
}

void selection_extend(term_t *t, int x, int y)
{
    if (!t)
        return;
    t->selection.end_x = x;
    t->selection.end_y = y;
    t->selection.active = true;
}

void selection_finish(term_t *t)
{
    if (!t)
        return;
    free(t->selection.text);
    t->selection.text = NULL;
    t->selection.text_len = 0;
    char *txt = sel_extract(t, &t->selection.text_len);
    t->selection.text = txt;
    if (!txt)
        t->selection.text_len = 0;
    t->selection.active = false;
}

void selection_clear(term_t *t)
{
    if (!t)
        return;
    free(t->selection.text);
    t->selection.text = NULL;
    t->selection.text_len = 0;
    t->selection.active = false;
}

char *selection_get_text(term_t *t)
{
    if (!t)
        return NULL;
    size_t n = 0;
    char *txt = sel_extract(t, &n);
    if (!txt) {
        /* string vazia malloc'd (caller sempre dá free) */
        txt = malloc(1);
        if (txt)
            txt[0] = '\0';
    }
    (void)n;
    return txt;
}
