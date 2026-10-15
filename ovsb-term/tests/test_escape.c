/* ovsb.term — test_escape — parser VT100/xterm sobre grid real */
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
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

static void feed(term_t *t, const char *s)
{
    escape_feed(t, s, strlen(s));
}

int main(void)
{
    term_t t;
    memset(&t, 0, sizeof(t));
    config_defaults(&t.config);
    /* pty fechado: respostas DA/DSR vão p/ fd -1 e falham silencioso */
    t.pty.master_fd = -1;
    scrollback_init(&t, 64);
    grid_init(&t, 24, 80);
    parser_init(&t.parser);

    /* 1. texto puro */
    feed(&t, "hello");
    CHECK(at(&t, 0, 0)->codepoint == 'h', "texto puro");

    /* 2. SGR cor */
    feed(&t, "\x1b[31mR\x1b[0mN");
    CHECK(at(&t, 5, 0)->fg == t.grid.palette[1], "SGR 31 fg");
    CHECK(at(&t, 6, 0)->fg == t.grid.default_fg, "SGR 0 reset");

    /* 3. CUP 1-based */
    feed(&t, "\x1b[5;10H*");
    CHECK(at(&t, 9, 4)->codepoint == '*', "CUP 5;10");

    /* 4. 256 cores + truecolor */
    feed(&t, "\x1b[38;5;196mA");
    CHECK(at(&t, 10, 4)->fg == t.grid.palette[196], "SGR 256");
    feed(&t, "\x1b[38;2;10;20;30mB");
    CHECK(at(&t, 11, 4)->fg == 0xFF0A141Eu, "SGR truecolor");

    /* 5. ED 2 limpa tudo */
    feed(&t, "\x1b[2J");
    CHECK(at(&t, 0, 0)->codepoint == ' ', "ED limpa");
    CHECK(at(&t, 9, 4)->codepoint == ' ', "ED limpa longe");

    /* 6. DECSET cursor hide/show */
    feed(&t, "\x1b[?25l");
    CHECK(!t.grid.cursor_visible, "DECRST 25 hide");
    feed(&t, "\x1b[?25h");
    CHECK(t.grid.cursor_visible, "DECSET 25 show");

    /* 7. OSC título */
    feed(&t, "\x1b]0;novo titulo\x07");
    CHECK(strcmp(t.title, "novo titulo") == 0, "OSC 0 titulo");

    /* 8. OSC 52 clipboard (Zm9v = foo) */
    feed(&t, "\x1b]52;c;Zm9v\x07");
    CHECK(t.clipboard.is_set && strcmp(t.clipboard.data, "foo") == 0,
          "OSC 52 clipboard");

    /* 9. UTF-8 multibyte */
    feed(&t, "\x1b[H\xc3\xa9"); /* é */
    CHECK(at(&t, 0, 0)->codepoint == 0xE9, "UTF-8 2 bytes");

    /* 10. DCS sixel descartado sem sujar */
    grid_clear_dirty(&t.grid);
    feed(&t, "\x1bPq...sixel...\x1b\\OK");
    /* cursor ficou em (1,0) após o 'é'; "OK" sai em 1,0/2,0 */
    CHECK(at(&t, 1, 0)->codepoint == 'O' &&
          at(&t, 2, 0)->codepoint == 'K', "DCS descartado");

    /* 11. save/restore cursor */
    feed(&t, "\x1b[8;20H" "\x1b" "7" "\x1b[H*" "\x1b" "8#");
    CHECK(at(&t, 0, 0)->codepoint == '*', "DECSC pos original");
    CHECK(at(&t, 19, 7)->codepoint == '#', "DECRC restaura");

    grid_cleanup(&t);
    scrollback_cleanup(&t);
    free(t.clipboard.data);
    printf(fails ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", fails);
    return fails ? 1 : 0;
}
