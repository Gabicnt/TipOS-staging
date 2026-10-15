/* ovsb.term — cursor.c — desenha cursor por estilo (moe moe kyun <3) */
#include "term.h"
#include "cursor.h"

/* preenche retângulo c/ clip em stride (pixels/linha) e altura máx */
static void fill_rect(uint32_t *px, int stride, int max_rows,
                      int x, int y, int w, int h, uint32_t color)
{
    int j, i;
    int y0 = y < 0 ? 0 : y;
    int x0 = x < 0 ? 0 : x;
    if (!px || w <= 0 || h <= 0)
        return;
    for (j = y0; j < y + h && j < max_rows; j++) {
        for (i = x0; i < x + w; i++) {
            if (i < 0 || i >= stride)
                continue;
            px[(size_t)j * (size_t)stride + (size_t)i] = color;
        }
    }
}

void cursor_draw(term_t *t, uint32_t *pixels, int stride,
                 int cell_x, int cell_y)
{
    grid_t *g;
    int cw, ch, px, py, max_rows;
    uint32_t color;
    cell_t *cell;

    if (!t || !pixels || stride <= 0)
        return;
    g = &t->grid;
    if (!g->cursor_visible)
        return;
    if (g->cursor_blink && !t->loop.blink_on)
        return; /* blink respeitado pelo loop */
    if (cell_x < 0 || cell_y < 0 || cell_x >= g->cols || cell_y >= g->rows)
        return;

    cw = (t->font.cell_width > 0) ? t->font.cell_width : 8;
    ch = (t->font.cell_height > 0) ? t->font.cell_height : 16;
    px = cell_x * cw;
    py = cell_y * ch;
    max_rows = g->rows * ch;

    cell = &g->cells[(size_t)cell_y * (size_t)g->cols + (size_t)cell_x];
    color = g->cursor_color ? g->cursor_color : cell->fg;
    if (color == 0)
        color = 0xFFFFFFFFu;

    switch (g->cursor_style) {
    case CURSOR_BLOCK:
        /* inverte: preenche célula c/ cor do cursor */
        fill_rect(pixels, stride, max_rows, px, py, cw, ch, color);
        break;
    case CURSOR_UNDERLINE:
        fill_rect(pixels, stride, max_rows, px, py + ch - 2, cw, 2, color);
        break;
    case CURSOR_BAR:
        fill_rect(pixels, stride, max_rows, px, py, 2, ch, color);
        break;
    case CURSOR_HOLLOW:
        fill_rect(pixels, stride, max_rows, px, py, cw, 1, color);
        fill_rect(pixels, stride, max_rows, px, py + ch - 1, cw, 1, color);
        fill_rect(pixels, stride, max_rows, px, py, 1, ch, color);
        fill_rect(pixels, stride, max_rows, px + cw - 1, py, 1, ch, color);
        break;
    default:
        fill_rect(pixels, stride, max_rows, px, py, cw, ch, color);
        break;
    }
}
