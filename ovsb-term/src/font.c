/* ovsb.term — font.c — fontes via fcft c/ fallback seguro (moe moe kyun <3) */
#include "term.h"
#include "font.h"
#include "glyph_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include) && __has_include(<fcft/fcft.h>)
#include <fcft/fcft.h>
#define HAVE_FCFT 1
#endif

#define FONT_CACHE_CAP 512
#define FONT_FALLBACK_W 8
#define FONT_FALLBACK_H 16

/* fcft_init uma única vez por processo */
static bool g_fcft_inited = false;

static bool ensure_fcft(void)
{
#ifdef HAVE_FCFT
    if (g_fcft_inited)
        return true;
    g_fcft_inited = fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_ERROR);
    return g_fcft_inited;
#else
    return false;
#endif
}

/* monta "family:size=N[extras]" p/ fontconfig */
static void build_spec(char *out, size_t outsz, const char *family,
                       int size, const char *extra)
{
    const char *fam = (family && family[0]) ? family : "monospace";
    int sz = size > 0 ? size : 12;
    if (extra && extra[0])
        snprintf(out, outsz, "%s:size=%d%s", fam, sz, extra);
    else
        snprintf(out, outsz, "%s:size=%d", fam, sz);
}

#ifdef HAVE_FCFT
/* cria uma variante; NULL se falhar */
static struct fcft_font *load_one(const char *spec)
{
    const char *names[1];
    names[0] = spec;
    return fcft_from_name(1, names, NULL);
}

/* destrói lista ignorando aliases p/ regular e NULLs */
static void destroy_unique(struct fcft_font *fonts[4])
{
    int i, j;
    for (i = 0; i < 4; i++) {
        struct fcft_font *f = fonts[i];
        bool seen = false;
        int k;
        if (!f)
            continue;
        for (k = 0; k < i; k++) {
            if (fonts[k] == f) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        for (j = i + 1; j < 4; j++)
            (void)j;
        fcft_destroy(f);
    }
}

/* (re)carrega as 4 variantes a partir do config; seta ready */
static void font_load(term_t *t)
{
    font_t *f = &t->font;
    struct fcft_font *fonts[4];
    char spec[192];
    const char *fam = t->config.font_family;
    int size = t->config.font_size > 0 ? t->config.font_size : 12;

    build_spec(spec, sizeof(spec), fam, size, "");
    fonts[0] = load_one(spec);
    if (!fonts[0]) {
        f->ready = false;
        f->regular = f->bold = f->italic = f->bold_italic = NULL;
        return;
    }
    build_spec(spec, sizeof(spec), fam, size, ":weight=bold");
    fonts[1] = load_one(spec);
    if (!fonts[1])
        fonts[1] = fonts[0];
    build_spec(spec, sizeof(spec), fam, size, ":slant=italic");
    fonts[2] = load_one(spec);
    if (!fonts[2])
        fonts[2] = fonts[0];
    build_spec(spec, sizeof(spec), fam, size, ":weight=bold:slant=italic");
    fonts[3] = load_one(spec);
    if (!fonts[3])
        fonts[3] = (fonts[1] != fonts[0]) ? fonts[1] : fonts[0];

    f->regular = fonts[0];
    f->bold = fonts[1];
    f->italic = fonts[2];
    f->bold_italic = fonts[3];
    f->ready = true;
    f->cell_width = fonts[0]->max_advance.x > 0 ? fonts[0]->max_advance.x
                                                : FONT_FALLBACK_W;
    f->cell_height = fonts[0]->height > 0 ? fonts[0]->height : FONT_FALLBACK_H;
    f->ascent = fonts[0]->ascent;
    f->descent = fonts[0]->descent;
    f->dpi = t->config.dpi > 0 ? t->config.dpi : 96;
}
#endif

void font_init(term_t *t)
{
    font_t *f;

    if (!t)
        return;
    f = &t->font;
    memset(f, 0, sizeof(*f));
    f->cell_width = FONT_FALLBACK_W;
    f->cell_height = FONT_FALLBACK_H;
    f->dpi = t->config.dpi > 0 ? t->config.dpi : 96;
    glyph_cache_init(&f->cache, FONT_CACHE_CAP);
    f->ready = false;
#ifdef HAVE_FCFT
    if (!ensure_fcft())
        return; /* sem fcft: fallback, render desenha caixas */
    font_load(t);
    if (!f->ready) {
        /* mantém cache válido porém vazio; cell fallback */
        f->cell_width = FONT_FALLBACK_W;
        f->cell_height = FONT_FALLBACK_H;
    }
#else
    (void)ensure_fcft;
#endif
}

void font_cleanup(term_t *t)
{
    font_t *f;

    if (!t)
        return;
    f = &t->font;
#ifdef HAVE_FCFT
    if (f->ready) {
        struct fcft_font *fonts[4];
        fonts[0] = f->regular;
        fonts[1] = f->bold;
        fonts[2] = f->italic;
        fonts[3] = f->bold_italic;
        destroy_unique(fonts);
        f->regular = f->bold = f->italic = f->bold_italic = NULL;
    }
#endif
    glyph_cache_done(&f->cache);
    f->ready = false;
}

void font_resize(term_t *t, int pixel_size)
{
    font_t *f;

    if (!t || pixel_size <= 0)
        return;
    f = &t->font;
    t->config.font_size = pixel_size;
#ifdef HAVE_FCFT
    if (f->ready) {
        struct fcft_font *fonts[4];
        fonts[0] = f->regular;
        fonts[1] = f->bold;
        fonts[2] = f->italic;
        fonts[3] = f->bold_italic;
        destroy_unique(fonts);
        f->regular = f->bold = f->italic = f->bold_italic = NULL;
        f->ready = false;
    }
    glyph_cache_done(&f->cache);
    glyph_cache_init(&f->cache, FONT_CACHE_CAP);
    if (!ensure_fcft())
        return;
    font_load(t);
#else
    glyph_cache_done(&f->cache);
    glyph_cache_init(&f->cache, FONT_CACHE_CAP);
#endif
}

const struct fcft_glyph *font_glyph(term_t *t, uint32_t cp, uint16_t attrs)
{
#ifdef HAVE_FCFT
    font_t *f;
    struct fcft_font *ff;
    uint32_t variant;
    const struct fcft_glyph *g = NULL;

    if (!t)
        return NULL;
    f = &t->font;
    if (!f->ready)
        return NULL;
    if ((attrs & ATTR_BOLD) && (attrs & ATTR_ITALIC)) {
        variant = 3;
        ff = f->bold_italic ? f->bold_italic : f->regular;
    } else if (attrs & ATTR_BOLD) {
        variant = 1;
        ff = f->bold ? f->bold : f->regular;
    } else if (attrs & ATTR_ITALIC) {
        variant = 2;
        ff = f->italic ? f->italic : f->regular;
    } else {
        variant = 0;
        ff = f->regular;
    }
    if (!ff)
        return NULL;
    if (glyph_cache_find(&f->cache, cp, variant, &g) && g)
        return g;
    g = fcft_rasterize_char_utf32(ff, cp, FCFT_SUBPIXEL_NONE);
    if (!g)
        return NULL;
    glyph_cache_put(&f->cache, cp, variant, g);
    return g;
#else
    (void)t;
    (void)cp;
    (void)attrs;
    return NULL;
#endif
}

int font_cell_width(font_t *f)
{
    if (!f || f->cell_width <= 0)
        return FONT_FALLBACK_W;
    return f->cell_width;
}

int font_cell_height(font_t *f)
{
    if (!f || f->cell_height <= 0)
        return FONT_FALLBACK_H;
    return f->cell_height;
}
