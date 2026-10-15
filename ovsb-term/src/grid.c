/* ovsb.term — grid.c — buffer de células do terminal */
#include "term.h"

#include <stdlib.h>
#include <string.h>

/* base 16 p/ palette[0..15] quando config não define */
static const uint32_t grid_base16[16] = {
    0xFF000000, 0xFFCD3131, 0xFF0DBC79, 0xFFE5E510,
    0xFF2472C8, 0xFFBC3FBC, 0xFF11A8CD, 0xFFE5E5E5,
    0xFF666666, 0xFFF14C4C, 0xFF23D18B, 0xFFF5F543,
    0xFF3B8EEA, 0xFFD670D6, 0xFF29B8DB, 0xFFE5E5E5,
};

static const unsigned cube_levels[6] = { 0, 95, 135, 175, 215, 255 };

/* gera palette xterm-256: 16 base + 216 cube + 24 gray */
static void palette_gen(grid_t *g, config_t *cfg)
{
    int i, r, gg, b;

    for (i = 0; i < 16; i++) {
        if (cfg != NULL && (cfg->palette[i] & 0xFF000000u) != 0)
            g->palette[i] = cfg->palette[i];
        else
            g->palette[i] = grid_base16[i];
    }
    for (r = 0; r < 6; r++) {
        for (gg = 0; gg < 6; gg++) {
            for (b = 0; b < 6; b++) {
                i = 16 + r * 36 + gg * 6 + b;
                g->palette[i] = 0xFF000000u |
                    (cube_levels[r] << 16) |
                    (cube_levels[gg] << 8) | cube_levels[b];
            }
        }
    }
    for (i = 0; i < 24; i++) {
        unsigned v = (unsigned)(8 + i * 10);
        g->palette[232 + i] = 0xFF000000u | (v << 16) | (v << 8) | v;
    }
}

/* célula em branco com cores default */
static cell_t blank_cell(const grid_t *g)
{
    cell_t c;
    c.codepoint = ' ';
    c.fg = g->default_fg;
    c.bg = g->default_bg;
    c.attrs = 0;
    c.width = 1;
    c._pad[0] = 0;
    c._pad[1] = 0;
    c._pad[2] = 0;
    return c;
}

/* preenche linha inteira com branco */
static void blank_row(grid_t *g, int row)
{
    int x;
    cell_t b = blank_cell(g);
    cell_t *dst = &g->cells[(size_t)row * (size_t)g->cols];
    for (x = 0; x < g->cols; x++)
        dst[x] = b;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void grid_mark_dirty(grid_t *g, int row)
{
    if (g == NULL || g->dirty == NULL)
        return;
    if (row < 0 || row >= g->rows)
        return;
    if (!g->dirty[row]) {
        g->dirty[row] = 1;
        g->dirty_count++;
    }
}

void grid_mark_all_dirty(grid_t *g)
{
    int y;
    if (g == NULL || g->dirty == NULL)
        return;
    for (y = 0; y < g->rows; y++)
        g->dirty[y] = 1;
    g->dirty_count = g->rows;
}

void grid_clear_dirty(grid_t *g)
{
    int y;
    if (g == NULL || g->dirty == NULL)
        return;
    for (y = 0; y < g->rows; y++)
        g->dirty[y] = 0;
    g->dirty_count = 0;
}

void grid_init(term_t *t, int rows, int cols)
{
    grid_t *g;
    int y;

    if (t == NULL)
        return;
    if (rows <= 0)
        rows = 24;
    if (cols <= 0)
        cols = 80;
    g = &t->grid;
    free(g->cells);
    free(g->dirty);
    g->cells = NULL;
    g->dirty = NULL;
    g->cells = (cell_t *)calloc((size_t)rows * (size_t)cols,
                                sizeof(cell_t));
    g->dirty = (uint8_t *)calloc((size_t)rows, sizeof(uint8_t));
    if (g->cells == NULL || g->dirty == NULL) {
        free(g->cells);
        free(g->dirty);
        g->cells = NULL;
        g->dirty = NULL;
        g->rows = 0;
        g->cols = 0;
        return;
    }
    g->rows = rows;
    g->cols = cols;
    /* defaults vêm da config se carregada, senão fallback */
    if (t->config.fg == 0 && t->config.bg == 0) {
        g->default_fg = 0xFFD4D4D4;
        g->default_bg = 0xFF1E1E1E;
    } else {
        g->default_fg = t->config.fg;
        g->default_bg = t->config.bg;
    }
    g->cursor_x = 0;
    g->cursor_y = 0;
    g->saved_x = 0;
    g->saved_y = 0;
    g->cursor_visible = true;
    g->cursor_style = t->config.cursor_style;
    g->cursor_blink = t->config.cursor_blink;
    g->cursor_color = t->config.cursor ? t->config.cursor : 0xFFFFFFFFu;
    g->cur_attrs = 0;
    g->cur_fg = g->default_fg;
    g->cur_bg = g->default_bg;
    palette_gen(g, &t->config);
    g->scroll_top = 0;
    g->scroll_bottom = rows - 1;
    g->wrap_next = false;
    g->origin_mode = false;
    g->insert_mode = false;
    g->auto_wrap = true; /* DECAWM default on */
    g->bracketed_paste = false;
    g->mouse_tracking = false;
    g->app_cursor = false;
    g->app_keypad = false;
    g->dirty_count = 0;
    g->last_char = 0;
    for (y = 0; y < rows; y++)
        blank_row(g, y);
    grid_mark_all_dirty(g);
}

void grid_cleanup(term_t *t)
{
    grid_t *g;
    if (t == NULL)
        return;
    g = &t->grid;
    free(g->cells);
    free(g->dirty);
    g->cells = NULL;
    g->dirty = NULL;
    g->rows = 0;
    g->cols = 0;
    g->dirty_count = 0;
}

void grid_resize(term_t *t, int rows, int cols)
{
    grid_t *g;
    cell_t *nw = NULL;
    uint8_t *nd = NULL;
    int old_rows, old_cols, y, x;
    int copy_rows, copy_cols, lost;

    if (t == NULL)
        return;
    if (rows <= 0)
        rows = 24;
    if (cols <= 0)
        cols = 80;
    g = &t->grid;
    if (g->cells == NULL) {
        grid_init(t, rows, cols);
        return;
    }
    if (rows == g->rows && cols == g->cols)
        return;
    old_rows = g->rows;
    old_cols = g->cols;
    nw = (cell_t *)calloc((size_t)rows * (size_t)cols, sizeof(cell_t));
    nd = (uint8_t *)calloc((size_t)rows, sizeof(uint8_t));
    if (nw == NULL || nd == NULL) {
        free(nw);
        free(nd);
        return;
    }
    /* linhas que somem por cima vão pro scrollback */
    lost = old_rows - rows;
    if (lost < 0)
        lost = 0;
    if (lost > old_rows)
        lost = old_rows;
    for (y = 0; y < lost; y++) {
        if (t->scrollback.lines != NULL)
            scrollback_push(t, &g->cells[(size_t)y * (size_t)old_cols]);
    }
    copy_rows = old_rows - lost;
    if (copy_rows > rows)
        copy_rows = rows;
    copy_cols = old_cols < cols ? old_cols : cols;
    /* preenche tudo com branco (novas cores default) */
    {
        cell_t b;
        b.codepoint = ' ';
        b.fg = g->default_fg;
        b.bg = g->default_bg;
        b.attrs = 0;
        b.width = 1;
        b._pad[0] = b._pad[1] = b._pad[2] = 0;
        for (y = 0; y < rows; y++) {
            int xx;
            for (xx = 0; xx < cols; xx++)
                nw[(size_t)y * (size_t)cols + (size_t)xx] = b;
        }
    }
    for (y = 0; y < copy_rows; y++) {
        for (x = 0; x < copy_cols; x++)
            nw[(size_t)y * (size_t)cols + (size_t)x] =
                g->cells[(size_t)(y + lost) * (size_t)old_cols + (size_t)x];
    }
    free(g->cells);
    free(g->dirty);
    g->cells = nw;
    g->dirty = nd;
    g->rows = rows;
    g->cols = cols;
    g->cursor_x = clampi(g->cursor_x, 0, cols - 1);
    g->cursor_y = clampi(g->cursor_y, 0, rows - 1);
    g->saved_x = clampi(g->saved_x, 0, cols - 1);
    g->saved_y = clampi(g->saved_y, 0, rows - 1);
    /* região de scroll: se era tela cheia, mantém cheia */
    if (g->scroll_top == 0 && g->scroll_bottom == old_rows - 1) {
        g->scroll_top = 0;
        g->scroll_bottom = rows - 1;
    } else {
        g->scroll_top = clampi(g->scroll_top, 0, rows - 1);
        g->scroll_bottom = clampi(g->scroll_bottom, 0, rows - 1);
        if (g->scroll_top > g->scroll_bottom) {
            g->scroll_top = 0;
            g->scroll_bottom = rows - 1;
        }
    }
    g->wrap_next = false;
    g->dirty_count = 0;
    grid_mark_all_dirty(g);
}

void grid_put_char(term_t *t, uint32_t cp, int width)
{
    grid_t *g;
    cell_t *row;
    int w;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || g->rows <= 0 || g->cols <= 0)
        return;
    if (cp < 0x20 || cp == 0x7F)
        return; /* controle não imprime aqui */
    if (width <= 0)
        return; /* combining: ignora */
    w = width > 2 ? 2 : width;

    /* wrap pendente: desce antes de escrever */
    if (g->wrap_next) {
        g->wrap_next = false;
        g->cursor_x = 0;
        if (g->cursor_y == g->scroll_bottom)
            grid_scroll_up(t, 1);
        else if (g->cursor_y < g->rows - 1)
            g->cursor_y++;
    }
    g->cursor_x = clampi(g->cursor_x, 0, g->cols - 1);
    g->cursor_y = clampi(g->cursor_y, 0, g->rows - 1);

    /* wide sem espaço no fim: quebra linha (se autowrap) */
    if (w == 2 && g->cursor_x == g->cols - 1) {
        if (g->auto_wrap) {
            g->cursor_x = 0;
            if (g->cursor_y == g->scroll_bottom)
                grid_scroll_up(t, 1);
            else if (g->cursor_y < g->rows - 1)
                g->cursor_y++;
        } else {
            g->cursor_x = g->cols - 2;
            if (g->cursor_x < 0)
                g->cursor_x = 0;
        }
    }

    if (g->insert_mode && w == 1)
        grid_insert_chars(t, 1);

    row = &g->cells[(size_t)g->cursor_y * (size_t)g->cols];

    /* limpa metade órfã de wide anterior */
    if ((row[g->cursor_x].attrs & ATTR_WIDE_TAIL) && g->cursor_x > 0) {
        cell_t b = blank_cell(g);
        row[g->cursor_x - 1] = b;
    }
    if ((row[g->cursor_x].attrs & ATTR_WIDE) &&
        g->cursor_x + 1 < g->cols) {
        cell_t b = blank_cell(g);
        row[g->cursor_x + 1] = b;
    }

    row[g->cursor_x].codepoint = cp;
    row[g->cursor_x].fg = g->cur_fg;
    row[g->cursor_x].bg = g->cur_bg;
    row[g->cursor_x].attrs = g->cur_attrs;
    row[g->cursor_x].width = (uint8_t)w;
    if (w == 2) {
        row[g->cursor_x].attrs |= ATTR_WIDE;
        if (g->cursor_x + 1 < g->cols) {
            cell_t b = blank_cell(g);
            row[g->cursor_x + 1] = b;
            row[g->cursor_x + 1].codepoint = 0;
            row[g->cursor_x + 1].attrs = ATTR_WIDE_TAIL;
            row[g->cursor_x + 1].width = 2;
        }
    }
    g->last_char = cp;
    grid_mark_dirty(g, g->cursor_y);

    g->cursor_x += w;
    if (g->cursor_x >= g->cols) {
        if (g->auto_wrap) {
            g->wrap_next = true;
            g->cursor_x = g->cols - 1;
        } else {
            g->cursor_x = g->cols - 1;
        }
    }
}

void grid_put_string(term_t *t, const char *utf8)
{
    size_t len, pos;

    if (t == NULL || utf8 == NULL)
        return;
    len = strlen(utf8);
    pos = 0;
    while (pos < len) {
        uint32_t cp = 0;
        int k = util_utf8_decode(utf8 + pos, len - pos, &cp);
        if (k <= 0) {
            pos++; /* byte incompleto no fim: pula */
            continue;
        }
        pos += (size_t)k;
        if (cp == '\n')
            grid_linefeed(t);
        else if (cp == '\r')
            grid_carriage_return(t);
        else if (cp == '\t')
            grid_tab(t);
        else if (cp == '\b')
            grid_backspace(t);
        else
            grid_put_char(t, cp, util_wcwidth(cp));
    }
}

void grid_linefeed(term_t *t)
{
    grid_t *g;
    if (t == NULL)
        return;
    g = &t->grid;
    g->wrap_next = false;
    if (g->cursor_y == g->scroll_bottom)
        grid_scroll_up(t, 1);
    else if (g->cursor_y < g->rows - 1)
        g->cursor_y++;
}

void grid_carriage_return(term_t *t)
{
    if (t == NULL)
        return;
    t->grid.cursor_x = 0;
    t->grid.wrap_next = false;
}

void grid_tab(term_t *t)
{
    grid_t *g;
    if (t == NULL)
        return;
    g = &t->grid;
    g->wrap_next = false;
    g->cursor_x = ((g->cursor_x / 8) + 1) * 8;
    if (g->cursor_x >= g->cols)
        g->cursor_x = g->cols - 1;
}

void grid_backspace(term_t *t)
{
    grid_t *g;
    if (t == NULL)
        return;
    g = &t->grid;
    if (g->wrap_next) {
        g->wrap_next = false;
        return;
    }
    if (g->cursor_x > 0)
        g->cursor_x--;
}

void grid_cursor_move(term_t *t, int dx, int dy)
{
    grid_t *g;
    int ylo, yhi;
    if (t == NULL)
        return;
    g = &t->grid;
    g->wrap_next = false;
    if (g->origin_mode) {
        ylo = g->scroll_top;
        yhi = g->scroll_bottom;
    } else {
        ylo = 0;
        yhi = g->rows - 1;
    }
    g->cursor_x = clampi(g->cursor_x + dx, 0, g->cols - 1);
    g->cursor_y = clampi(g->cursor_y + dy, ylo, yhi);
}

void grid_cursor_set(term_t *t, int x, int y)
{
    grid_t *g;
    if (t == NULL)
        return;
    g = &t->grid;
    g->wrap_next = false;
    if (g->origin_mode)
        y += g->scroll_top;
    g->cursor_x = clampi(x, 0, g->cols - 1);
    if (g->origin_mode)
        g->cursor_y = clampi(y, g->scroll_top, g->scroll_bottom);
    else
        g->cursor_y = clampi(y, 0, g->rows - 1);
}

void grid_cursor_save(term_t *t)
{
    if (t == NULL)
        return;
    t->grid.saved_x = t->grid.cursor_x;
    t->grid.saved_y = t->grid.cursor_y;
}

void grid_cursor_restore(term_t *t)
{
    grid_t *g;
    if (t == NULL)
        return;
    g = &t->grid;
    g->wrap_next = false;
    g->cursor_x = clampi(g->saved_x, 0, g->cols - 1);
    g->cursor_y = clampi(g->saved_y, 0, g->rows - 1);
}

void grid_erase_display(term_t *t, int mode)
{
    grid_t *g;
    int y, x;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL)
        return;
    if (mode == 3) {
        scrollback_clear(t);
        mode = 2;
    }
    if (mode == 2) {
        for (y = 0; y < g->rows; y++)
            blank_row(g, y);
        grid_mark_all_dirty(g);
        return;
    }
    if (mode == 1) {
        /* do início até o cursor */
        for (y = 0; y < g->cursor_y; y++)
            blank_row(g, y);
        for (y = 0; y < g->cursor_y; y++)
            grid_mark_dirty(g, y);
        if (g->cursor_y >= 0 && g->cursor_y < g->rows) {
            cell_t *row =
                &g->cells[(size_t)g->cursor_y * (size_t)g->cols];
            cell_t b = blank_cell(g);
            for (x = 0; x <= g->cursor_x && x < g->cols; x++)
                row[x] = b;
            grid_mark_dirty(g, g->cursor_y);
        }
        return;
    }
    /* mode 0 (default): do cursor até o fim */
    if (g->cursor_y >= 0 && g->cursor_y < g->rows) {
        cell_t *row = &g->cells[(size_t)g->cursor_y * (size_t)g->cols];
        cell_t b = blank_cell(g);
        for (x = g->cursor_x; x < g->cols; x++)
            row[x] = b;
        grid_mark_dirty(g, g->cursor_y);
    }
    for (y = g->cursor_y + 1; y < g->rows; y++) {
        blank_row(g, y);
        grid_mark_dirty(g, y);
    }
}

void grid_erase_line(term_t *t, int mode)
{
    grid_t *g;
    cell_t *row, b;
    int x;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL)
        return;
    if (g->cursor_y < 0 || g->cursor_y >= g->rows)
        return;
    row = &g->cells[(size_t)g->cursor_y * (size_t)g->cols];
    b = blank_cell(g);
    if (mode == 1) {
        for (x = 0; x <= g->cursor_x && x < g->cols; x++)
            row[x] = b;
    } else if (mode == 2) {
        for (x = 0; x < g->cols; x++)
            row[x] = b;
    } else {
        for (x = g->cursor_x; x < g->cols; x++)
            row[x] = b;
    }
    grid_mark_dirty(g, g->cursor_y);
}

void grid_erase_chars(term_t *t, int n)
{
    grid_t *g;
    cell_t *row, b;
    int x, end;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || n <= 0)
        return;
    if (g->cursor_y < 0 || g->cursor_y >= g->rows)
        return;
    row = &g->cells[(size_t)g->cursor_y * (size_t)g->cols];
    b = blank_cell(g);
    end = g->cursor_x + n;
    if (end > g->cols)
        end = g->cols;
    for (x = g->cursor_x; x < end; x++)
        row[x] = b;
    grid_mark_dirty(g, g->cursor_y);
}

void grid_insert_lines(term_t *t, int n)
{
    grid_t *g;
    int y;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || n <= 0)
        return;
    if (g->cursor_y < g->scroll_top || g->cursor_y > g->scroll_bottom)
        return;
    if (n > g->scroll_bottom - g->cursor_y + 1)
        n = g->scroll_bottom - g->cursor_y + 1;
    for (y = g->scroll_bottom; y >= g->cursor_y + n; y--) {
        memcpy(&g->cells[(size_t)y * (size_t)g->cols],
               &g->cells[(size_t)(y - n) * (size_t)g->cols],
               (size_t)g->cols * sizeof(cell_t));
    }
    for (y = g->cursor_y; y < g->cursor_y + n; y++)
        blank_row(g, y);
    for (y = g->cursor_y; y <= g->scroll_bottom; y++)
        grid_mark_dirty(g, y);
}

void grid_delete_lines(term_t *t, int n)
{
    grid_t *g;
    int y;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || n <= 0)
        return;
    if (g->cursor_y < g->scroll_top || g->cursor_y > g->scroll_bottom)
        return;
    if (n > g->scroll_bottom - g->cursor_y + 1)
        n = g->scroll_bottom - g->cursor_y + 1;
    for (y = g->cursor_y; y <= g->scroll_bottom - n; y++) {
        memcpy(&g->cells[(size_t)y * (size_t)g->cols],
               &g->cells[(size_t)(y + n) * (size_t)g->cols],
               (size_t)g->cols * sizeof(cell_t));
    }
    for (y = g->scroll_bottom - n + 1; y <= g->scroll_bottom; y++)
        blank_row(g, y);
    for (y = g->cursor_y; y <= g->scroll_bottom; y++)
        grid_mark_dirty(g, y);
}

void grid_insert_chars(term_t *t, int n)
{
    grid_t *g;
    cell_t *row, b;
    int x;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || n <= 0)
        return;
    if (g->cursor_y < 0 || g->cursor_y >= g->rows)
        return;
    if (g->cursor_x < 0 || g->cursor_x >= g->cols)
        return;
    row = &g->cells[(size_t)g->cursor_y * (size_t)g->cols];
    if (n > g->cols - g->cursor_x)
        n = g->cols - g->cursor_x;
    for (x = g->cols - 1; x >= g->cursor_x + n; x--)
        row[x] = row[x - n];
    b = blank_cell(g);
    for (x = g->cursor_x; x < g->cursor_x + n; x++)
        row[x] = b;
    grid_mark_dirty(g, g->cursor_y);
}

void grid_delete_chars(term_t *t, int n)
{
    grid_t *g;
    cell_t *row, b;
    int x;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || n <= 0)
        return;
    if (g->cursor_y < 0 || g->cursor_y >= g->rows)
        return;
    if (g->cursor_x < 0 || g->cursor_x >= g->cols)
        return;
    row = &g->cells[(size_t)g->cursor_y * (size_t)g->cols];
    if (n > g->cols - g->cursor_x)
        n = g->cols - g->cursor_x;
    for (x = g->cursor_x; x < g->cols - n; x++)
        row[x] = row[x + n];
    b = blank_cell(g);
    for (x = g->cols - n; x < g->cols; x++)
        row[x] = b;
    grid_mark_dirty(g, g->cursor_y);
}

void grid_scroll_up(term_t *t, int n)
{
    grid_t *g;
    int y, top, bot;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || n <= 0)
        return;
    top = clampi(g->scroll_top, 0, g->rows - 1);
    bot = clampi(g->scroll_bottom, 0, g->rows - 1);
    if (top > bot)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    /* tela cheia: linhas que saem vão pro scrollback */
    if (top == 0 && bot == g->rows - 1) {
        for (y = 0; y < n; y++) {
            if (t->scrollback.lines != NULL)
                scrollback_push(t,
                    &g->cells[(size_t)(top + y) * (size_t)g->cols]);
        }
    }
    for (y = top; y <= bot - n; y++) {
        memcpy(&g->cells[(size_t)y * (size_t)g->cols],
               &g->cells[(size_t)(y + n) * (size_t)g->cols],
               (size_t)g->cols * sizeof(cell_t));
    }
    for (y = bot - n + 1; y <= bot; y++)
        blank_row(g, y);
    for (y = top; y <= bot; y++)
        grid_mark_dirty(g, y);
}

void grid_scroll_down(term_t *t, int n)
{
    grid_t *g;
    int y, top, bot;

    if (t == NULL)
        return;
    g = &t->grid;
    if (g->cells == NULL || n <= 0)
        return;
    top = clampi(g->scroll_top, 0, g->rows - 1);
    bot = clampi(g->scroll_bottom, 0, g->rows - 1);
    if (top > bot)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    for (y = bot; y >= top + n; y--) {
        memcpy(&g->cells[(size_t)y * (size_t)g->cols],
               &g->cells[(size_t)(y - n) * (size_t)g->cols],
               (size_t)g->cols * sizeof(cell_t));
    }
    for (y = top; y < top + n; y++)
        blank_row(g, y);
    for (y = top; y <= bot; y++)
        grid_mark_dirty(g, y);
}

void grid_set_fg(term_t *t, uint32_t color)
{
    if (t == NULL)
        return;
    t->grid.cur_fg = color;
}

void grid_set_bg(term_t *t, uint32_t color)
{
    if (t == NULL)
        return;
    t->grid.cur_bg = color;
}

void grid_reset_attrs(term_t *t)
{
    grid_t *g;
    if (t == NULL)
        return;
    g = &t->grid;
    g->cur_attrs = 0;
    g->cur_fg = g->default_fg;
    g->cur_bg = g->default_bg;
}
