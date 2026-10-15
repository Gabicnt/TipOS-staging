/* ovsb.term — test_scrollback — ring buffer de histórico */
#include "term.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("ok - %s\n", name); } \
    else { printf("FAIL - %s\n", name); fails++; } \
} while (0)

int main(void)
{
    term_t t;
    memset(&t, 0, sizeof(t));
    config_defaults(&t.config);
    scrollback_init(&t, 8);
    grid_init(&t, 4, 6);

    /* empurra 5 linhas de 6 células */
    for (int i = 0; i < 5; i++) {
        cell_t line[6];
        for (int x = 0; x < 6; x++) {
            line[x].codepoint = (uint32_t)('A' + i);
            line[x].fg = 1;
            line[x].bg = 2;
            line[x].attrs = 0;
            line[x].width = 1;
        }
        scrollback_push(&t, line);
    }
    CHECK(t.scrollback.count == 5, "count 5");

    cell_t *oldest = scrollback_get(&t.scrollback, 0);
    CHECK(oldest && oldest[0].codepoint == 'A', "mais antiga = A");
    cell_t *newest = scrollback_get(&t.scrollback, 4);
    CHECK(newest && newest[0].codepoint == 'E', "mais nova = E");

    /* overflow: capacity 8, empurra mais 6 → total 11, fica 8 */
    for (int i = 0; i < 6; i++) {
        cell_t line[6];
        for (int x = 0; x < 6; x++) {
            line[x].codepoint = (uint32_t)('a' + i);
            line[x].fg = 1;
            line[x].bg = 2;
            line[x].attrs = 0;
            line[x].width = 1;
        }
        scrollback_push(&t, line);
    }
    CHECK(t.scrollback.count == 8, "clamp capacity");
    oldest = scrollback_get(&t.scrollback, 0);
    CHECK(oldest && oldest[0].codepoint == 'D', "ring sobrescreve");

    /* scroll view + clear */
    scrollback_scroll(&t, 3);
    CHECK(t.scrollback.view_offset == 3, "view offset");
    scrollback_scroll(&t, 100);
    CHECK(t.scrollback.view_offset == 8, "view clamp");
    scrollback_clear(&t);
    CHECK(t.scrollback.count == 0 && t.scrollback.view_offset == 0, "clear");
    CHECK(scrollback_get(&t.scrollback, 0) == NULL, "get vazio NULL");

    grid_cleanup(&t);
    scrollback_cleanup(&t);
    printf(fails ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", fails);
    return fails ? 1 : 0;
}
