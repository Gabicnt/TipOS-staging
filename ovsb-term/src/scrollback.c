/* ovsb.term — scrollback.c — ring buffer de linhas */
#include "term.h"

#include <stdlib.h>
#include <string.h>

void scrollback_init(term_t *t, int capacity)
{
    scrollback_t *sb;
    int cols;

    if (t == NULL)
        return;
    sb = &t->scrollback;
    sb->lines = NULL;
    sb->capacity = 0;
    sb->head = 0;
    sb->count = 0;
    sb->view_offset = 0;
    sb->cols = 0;
    if (capacity <= 0)
        return;
    cols = t->grid.cols > 0 ? t->grid.cols : 80;
    sb->lines = (cell_t *)calloc((size_t)capacity * (size_t)cols,
                                 sizeof(cell_t));
    if (sb->lines == NULL)
        return;
    sb->capacity = capacity;
    sb->cols = cols;
}

void scrollback_cleanup(term_t *t)
{
    scrollback_t *sb;

    if (t == NULL)
        return;
    sb = &t->scrollback;
    free(sb->lines);
    sb->lines = NULL;
    sb->capacity = 0;
    sb->head = 0;
    sb->count = 0;
    sb->view_offset = 0;
    sb->cols = 0;
}

void scrollback_push(term_t *t, cell_t *line)
{
    scrollback_t *sb;
    cell_t *dst;
    int gcols, m, i;

    if (t == NULL || line == NULL)
        return;
    sb = &t->scrollback;
    if (sb->lines == NULL || sb->capacity <= 0 || sb->cols <= 0)
        return;
    gcols = t->grid.cols > 0 ? t->grid.cols : sb->cols;
    dst = &sb->lines[(size_t)sb->head * (size_t)sb->cols];
    if (gcols == sb->cols) {
        memcpy(dst, line, (size_t)sb->cols * sizeof(cell_t));
    } else {
        /* larguras divergem (resize): copia o mínimo, completa resto */
        m = gcols < sb->cols ? gcols : sb->cols;
        memcpy(dst, line, (size_t)m * sizeof(cell_t));
        for (i = m; i < sb->cols; i++) {
            dst[i].codepoint = ' ';
            dst[i].fg = line[0].fg;
            dst[i].bg = line[0].bg;
            dst[i].attrs = 0;
            dst[i].width = 1;
        }
    }
    sb->head = (sb->head + 1) % sb->capacity;
    if (sb->count < sb->capacity)
        sb->count++;
    sb->view_offset = 0; /* volta pro bottom ao chegar linha nova */
}

void scrollback_scroll(term_t *t, int delta)
{
    scrollback_t *sb;

    if (t == NULL)
        return;
    sb = &t->scrollback;
    sb->view_offset += delta;
    if (sb->view_offset < 0)
        sb->view_offset = 0;
    if (sb->view_offset > sb->count)
        sb->view_offset = sb->count;
}

void scrollback_clear(term_t *t)
{
    scrollback_t *sb;

    if (t == NULL)
        return;
    sb = &t->scrollback;
    sb->count = 0;
    sb->head = 0;
    sb->view_offset = 0;
}

cell_t *scrollback_get(scrollback_t *sb, int offset)
{
    int idx;

    if (sb == NULL || sb->lines == NULL)
        return NULL;
    if (sb->capacity <= 0 || sb->cols <= 0)
        return NULL;
    if (offset < 0 || offset >= sb->count)
        return NULL;
    idx = (sb->head - sb->count + offset) % sb->capacity;
    if (idx < 0)
        idx += sb->capacity;
    return &sb->lines[(size_t)idx * (size_t)sb->cols];
}
