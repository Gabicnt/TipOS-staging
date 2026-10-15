/* ovsb.term — glyph_cache.h — LRU de glifos p/ font.c (moe moe kyun <3) */
#ifndef OVSB_GLYPH_CACHE_H
#define OVSB_GLYPH_CACHE_H

#include "term.h"

/* init/done já declarados em term.h; aqui só find/put (não editar term.h) */
int glyph_cache_find(glyph_cache_t *c, uint32_t cp, uint32_t variant,
                     const struct fcft_glyph **out);
void glyph_cache_put(glyph_cache_t *c, uint32_t cp, uint32_t variant,
                     const struct fcft_glyph *glyph);

#endif /* OVSB_GLYPH_CACHE_H */
