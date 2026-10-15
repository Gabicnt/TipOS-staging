/* ovsb.term — glyph_cache.c — LRU linear de glifos (moe moe kyun <3) */
#include "term.h"
#include "glyph_cache.h"

#include <stdlib.h>

/* Capacidade padrão quando caller passa valor inválido */
#define GLYPH_CACHE_DEFAULT_CAP 512

void glyph_cache_init(glyph_cache_t *c, int capacity)
{
    if (!c)
        return;
    if (capacity <= 0)
        capacity = GLYPH_CACHE_DEFAULT_CAP;
    c->entries = (glyph_entry_t *)calloc((size_t)capacity, sizeof(glyph_entry_t));
    if (!c->entries) {
        c->capacity = 0;
        c->count = 0;
        c->clock = 0;
        return;
    }
    c->capacity = capacity;
    c->count = 0;
    c->clock = 0;
}

void glyph_cache_done(glyph_cache_t *c)
{
    if (!c)
        return;
    free(c->entries);
    c->entries = NULL;
    c->capacity = 0;
    c->count = 0;
    c->clock = 0;
}

int glyph_cache_find(glyph_cache_t *c, uint32_t cp, uint32_t variant,
                     const struct fcft_glyph **out)
{
    int i;

    if (out)
        *out = NULL;
    if (!c || !c->entries)
        return 0;
    for (i = 0; i < c->count; i++) {
        if (c->entries[i].codepoint == cp && c->entries[i].variant == variant) {
            c->clock++;
            c->entries[i].last_used = c->clock;
            if (out)
                *out = c->entries[i].glyph;
            return 1;
        }
    }
    return 0;
}

void glyph_cache_put(glyph_cache_t *c, uint32_t cp, uint32_t variant,
                     const struct fcft_glyph *glyph)
{
    int i, victim;

    if (!c || !c->entries || c->capacity <= 0 || !glyph)
        return;
    /* atualiza entrada existente */
    for (i = 0; i < c->count; i++) {
        if (c->entries[i].codepoint == cp && c->entries[i].variant == variant) {
            c->entries[i].glyph = glyph;
            c->clock++;
            c->entries[i].last_used = c->clock;
            return;
        }
    }
    c->clock++;
    if (c->count < c->capacity) {
        c->entries[c->count].glyph = glyph;
        c->entries[c->count].codepoint = cp;
        c->entries[c->count].variant = variant;
        c->entries[c->count].last_used = c->clock;
        c->count++;
        return;
    }
    /* cheio: expulsa o menos recentemente usado */
    victim = 0;
    for (i = 1; i < c->count; i++) {
        if (c->entries[i].last_used < c->entries[victim].last_used)
            victim = i;
    }
    c->entries[victim].glyph = glyph;
    c->entries[victim].codepoint = cp;
    c->entries[victim].variant = variant;
    c->entries[victim].last_used = c->clock;
}
