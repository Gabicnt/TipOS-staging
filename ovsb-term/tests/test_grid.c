/* ovsb.term — test_grid — teste unitário do grid (sem deps externas) */
#include "term.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("ok - %s\n", name); } \
    else { printf("FAIL - %s\n", name); fails++; } \
} while (0)

static cell_t *at(term_t *t, int x, int y)
{
    return &t->grid.cells[(size_t)y * (size_t)t->grid.cols + (size_t)x];
}

int main(void)
{
    term_t t;
    memset(&t, 0, sizeof(t));
    config_defaults(&t.config);
    scrollback_init(&t, 64);
    grid_init(&t, 5, 10);

    /* 1. escrita básica */
    grid_put_string(&t, "hello");
    CHECK(at(&t, 0, 0)->codepoint == 'h', "put h");
    CHECK(at(&t, 4, 0)->codepoint == 'o', "put o");
    CHECK(t.grid.cursor_x == 5, "cursor avanca");

    /* 2. fg via set_fg */
    grid_set_fg(&t, 0xFFFF0000u);
    grid_put_string(&t, "X");
    CHECK(at(&t, 5, 0)->fg == 0xFFFF0000u, "fg vermelho");
    grid_reset_attrs(&t);

    /* 3. cursor set + escrita posicionada */
    grid_cursor_set(&t, 2, 3);
    grid_put_string(&t, "Z");
    CHECK(at(&t, 2, 3)->codepoint == 'Z', "cursor_set escreve");
    CHECK(t.grid.cursor_x == 3 && t.grid.cursor_y == 3, "cursor pos");

    /* 4. erase line mode 2 */
    grid_cursor_set(&t, 0, 3);
    grid_erase_line(&t, 2);
    CHECK(at(&t, 2, 3)->codepoint == ' ', "erase_line limpa");

    /* 5. wide char ocupa 2 células */
    grid_cursor_set(&t, 0, 4);
    grid_put_char(&t, 0x4E2Du, 2); /* 中 */
    CHECK(at(&t, 0, 4)->codepoint == 0x4E2Du, "wide base");
    CHECK(at(&t, 1, 4)->attrs & ATTR_WIDE_TAIL, "wide tail");

    /* 6. scroll empurra p/ scrollback */
    grid_cursor_set(&t, 0, 0);
    grid_erase_display(&t, 2);
    for (int i = 0; i < 7; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "lin%d", i);
        grid_cursor_set(&t, 0, 4);
        grid_put_string(&t, buf);
        grid_linefeed(&t);
    }
    CHECK(t.scrollback.count > 0, "scrollback recebeu linhas");

    /* 7. dirty tracking */
    grid_clear_dirty(&t.grid);
    CHECK(t.grid.dirty_count == 0, "clear dirty");
    grid_cursor_set(&t, 0, 0);
    grid_put_string(&t, "d");
    CHECK(t.grid.dirty_count > 0, "put marca dirty");

    /* 8. palette xterm-256 gerada */
    CHECK(t.grid.palette[1] == 0xFFCD3131u, "palette[1] vermelho");
    CHECK(t.grid.palette[16] == 0xFF000000u, "palette[16] preto cube");
    CHECK(t.grid.palette[231] != 0, "palette cube fim");

    grid_cleanup(&t);
    scrollback_cleanup(&t);
    printf(fails ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", fails);
    return fails ? 1 : 0;
}
