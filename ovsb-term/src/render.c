/* ovsb.term — render.c — compõe grid no shm (moe moe kyun <3) */
#include "term.h"
#include "render.h"

#include <stdlib.h>
#include <string.h>

#if defined(__has_include) && __has_include(<pixman.h>)
#include <pixman.h>
#define HAVE_PIXMAN 1
#endif
#if defined(__has_include) && __has_include(<fcft/fcft.h>)
#include <fcft/fcft.h>
#define HAVE_FCFT_H 1
#endif

#define FALLBACK_CW 8
#define FALLBACK_CH 16

static int cell_w_of(term_t *t)
{
    return (t->font.ready && t->font.cell_width > 0) ? t->font.cell_width
                                                    : FALLBACK_CW;
}

static int cell_h_of(term_t *t)
{
    return (t->font.ready && t->font.cell_height > 0) ? t->font.cell_height
                                                     : FALLBACK_CH;
}

/* destrói conteúdo de um slot via cópia no heap (destroy dá free) */
static void slot_destroy(shm_buffer_t *slot)
{
    shm_buffer_t *tmp;

    if (!slot || !slot->valid)
        return;
    tmp = (shm_buffer_t *)malloc(sizeof(*tmp));
    if (!tmp)
        return;
    memcpy(tmp, slot, sizeof(*tmp));
    memset(slot, 0, sizeof(*slot));
    shm_buffer_destroy(tmp);
}

/* cria conteúdo de um slot; false = sem shm */
static bool slot_create(term_t *t, shm_buffer_t *slot, int w, int h)
{
    shm_buffer_t *tmp;

    memset(slot, 0, sizeof(*slot));
    if (!t->wl.shm || w <= 0 || h <= 0)
        return false;
    tmp = shm_buffer_create(t->wl.shm, w, h);
    if (!tmp)
        return false;
    memcpy(slot, tmp, sizeof(*slot));
    free(tmp);
    return slot->valid && slot->pixels;
}

static void alloc_buffers(term_t *t, int w, int h)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (!slot_create(t, &t->render.buffers[i], w, h)) {
            memset(&t->render.buffers[i], 0, sizeof(shm_buffer_t));
        }
    }
    t->render.current = 0;
}

void render_init(term_t *t)
{
    int w, h;

    if (!t)
        return;
    w = t->wl.width > 0 ? t->wl.width : 800;
    h = t->wl.height > 0 ? t->wl.height : 600;
    t->render.width = w;
    t->render.height = h;
    t->render.current = 0;
    t->render.attached = false;
    alloc_buffers(t, w, h);
}

void render_resize(term_t *t, int width, int height)
{
    int i;

    if (!t || width <= 0 || height <= 0)
        return;
    for (i = 0; i < 2; i++)
        slot_destroy(&t->render.buffers[i]);
    t->render.width = width;
    t->render.height = height;
    t->wl.width = width;
    t->wl.height = height;
    alloc_buffers(t, width, height);
}

/* aplica DIM: reduz rgb pela metade */
static uint32_t apply_dim(uint32_t argb)
{
    uint32_t a = (argb >> 24) & 0xFFu;
    uint32_t r = ((argb >> 16) & 0xFFu) / 2u;
    uint32_t gg = ((argb >> 8) & 0xFFu) / 2u;
    uint32_t b = (argb & 0xFFu) / 2u;
    return (a << 24) | (r << 16) | (gg << 8) | b;
}

static void fill_cell_bg(uint32_t *px, int stride, int buf_w, int buf_h,
                         int x, int y, int w, int h, uint32_t color)
{
    int j, i;
    int x1 = x + w, y1 = y + h;
    if (!px)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > buf_w)
        x1 = buf_w;
    if (y1 > buf_h)
        y1 = buf_h;
    for (j = y; j < y1; j++) {
        for (i = x; i < x1; i++)
            px[(size_t)j * (size_t)stride + (size_t)i] = color;
    }
}

/* mistura fg sobre dst usando alpha 0-255 */
static uint32_t blend_over(uint32_t dst, uint32_t fg, uint32_t alpha)
{
    uint32_t inv = 255u - alpha;
    uint32_t dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
    uint32_t fr = (fg >> 16) & 0xFFu, fg_ = (fg >> 8) & 0xFFu, fb = fg & 0xFFu;
    uint32_t r = (fr * alpha + dr * inv) / 255u;
    uint32_t g = (fg_ * alpha + dg * inv) / 255u;
    uint32_t b = (fb * alpha + db * inv) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

#ifdef HAVE_FCFT_H
/* desenha glifo ARGB manual: alpha = canal A do pixman */
static void blit_glyph(term_t *t, uint32_t *px, int stride, int buf_w, int buf_h,
                       const struct fcft_glyph *gl, int ox, int oy,
                       uint32_t fg, bool bold)
{
    int pass;
    (void)t;
    if (!gl || !gl->pix)
        return;
#ifdef HAVE_PIXMAN
    {
        uint32_t *src = pixman_image_get_data(gl->pix);
        int sw = pixman_image_get_width(gl->pix);
        int sh = pixman_image_get_height(gl->pix);
        int sstride = pixman_image_get_stride(gl->pix) / 4;
        if (!src || sw <= 0 || sh <= 0)
            return;
        for (pass = 0; pass < (bold ? 2 : 1); pass++) {
            int gy, gx;
            for (gy = 0; gy < sh; gy++) {
                for (gx = 0; gx < sw; gx++) {
                    uint32_t s = src[(size_t)gy * (size_t)sstride + (size_t)gx];
                    uint32_t a;
                    int dx = ox + gl->x + gx + pass;
                    int dy = oy + gl->y + gy;
                    if (gl->is_color_glyph) {
                        if (dx < 0 || dy < 0 || dx >= buf_w || dy >= buf_h)
                            continue;
                        px[(size_t)dy * (size_t)stride + (size_t)dx] = s;
                        continue;
                    }
                    a = (s >> 24) & 0xFFu;
                    if (a == 0)
                        continue;
                    if (dx < 0 || dy < 0 || dx >= buf_w || dy >= buf_h)
                        continue;
                    px[(size_t)dy * (size_t)stride + (size_t)dx] =
                        blend_over(px[(size_t)dy * (size_t)stride + (size_t)dx],
                                   fg, a);
                }
            }
        }
    }
#else
    (void)ox;
    (void)oy;
    (void)fg;
    (void)bold;
#endif
}
#endif

/* desenha uma linha decorativa (underline/strike) */
static void draw_hline(uint32_t *px, int stride, int buf_w, int buf_h,
                       int x, int y, int w, uint32_t fg)
{
    int i;
    if (!px || y < 0 || y >= buf_h)
        return;
    for (i = 0; i < w; i++) {
        int dx = x + i;
        if (dx < 0 || dx >= buf_w)
            continue;
        px[(size_t)y * (size_t)stride + (size_t)dx] = fg;
    }
}

void render_update(term_t *t)
{
    grid_t *g;
    shm_buffer_t *buf;
    uint32_t *px;
    int stride, buf_w, buf_h;
    int cw, ch, row, col;

    if (!t)
        return;
    g = &t->grid;
    if (!g->cells || g->rows <= 0 || g->cols <= 0)
        return;
    if (g->dirty_count <= 0)
        return;

    buf = &t->render.buffers[t->render.current & 1];
    if (!buf->valid || !buf->pixels) {
        grid_clear_dirty(g); /* sem shm: só limpa dirty */
        return;
    }
    px = buf->pixels;
    stride = buf->stride > 0 ? buf->stride / 4 : buf->width;
    if (stride <= 0)
        stride = buf->width;
    buf_w = buf->width;
    buf_h = buf->height;
    cw = cell_w_of(t);
    ch = cell_h_of(t);

    for (row = 0; row < g->rows; row++) {
        if (!g->dirty[row])
            continue;
        for (col = 0; col < g->cols; col++) {
            cell_t *c = &g->cells[(size_t)row * (size_t)g->cols + (size_t)col];
            uint32_t fg = c->fg, bg = c->bg;
            int ox = col * cw, oy = row * ch;
            bool wide_tail = (c->attrs & ATTR_WIDE_TAIL) != 0;
#ifdef HAVE_FCFT_H
            const struct fcft_glyph *gl = NULL;
#endif
            if (c->attrs & ATTR_REVERSE) {
                uint32_t tmpc = fg;
                fg = bg;
                bg = tmpc;
            }
            if (c->attrs & ATTR_DIM)
                fg = apply_dim(fg);
            if (c->attrs & ATTR_HIDDEN)
                fg = bg;
            fill_cell_bg(px, stride, buf_w, buf_h, ox, oy,
                         c->width == 2 ? cw * 2 : cw, ch, bg);
            if (wide_tail)
                continue; /* pula WIDE_TAIL */
#ifdef HAVE_FCFT_H
            if (c->codepoint != 0 && c->codepoint != 0x20) {
                gl = font_glyph(t, c->codepoint, c->attrs);
                if (gl) {
                    blit_glyph(t, px, stride, buf_w, buf_h, gl, ox, oy, fg,
                               (c->attrs & ATTR_BOLD) != 0);
                }
            }
#else
            (void)fg;
#endif
            if (c->attrs & ATTR_UNDERLINE)
                draw_hline(px, stride, buf_w, buf_h, ox, oy + ch - 1, cw, fg);
            if (c->attrs & ATTR_STRIKE)
                draw_hline(px, stride, buf_w, buf_h, ox, oy + ch / 2, cw, fg);
            if (c->attrs & ATTR_OVERLINE)
                draw_hline(px, stride, buf_w, buf_h, ox, oy, cw, fg);
        }
    }

    render_cursor(t);
    if (t->wl.display && t->wl.surface)
        t->render.attached = wl_attach(t);
    t->render.current ^= 1;
    grid_clear_dirty(g);
}

void render_full(term_t *t)
{
    if (!t)
        return;
    grid_mark_all_dirty(&t->grid);
    render_update(t);
}

void render_cursor(term_t *t)
{
    shm_buffer_t *buf;
    int stride;

    if (!t)
        return;
    buf = &t->render.buffers[t->render.current & 1];
    if (!buf->valid || !buf->pixels)
        return;
    stride = buf->stride > 0 ? buf->stride / 4 : buf->width;
    if (stride <= 0)
        stride = buf->width;
    cursor_draw(t, buf->pixels, stride, t->grid.cursor_x, t->grid.cursor_y);
}

void render_cleanup(term_t *t)
{
    int i;

    if (!t)
        return;
    for (i = 0; i < 2; i++)
        slot_destroy(&t->render.buffers[i]);
    t->render.current = 0;
    t->render.width = 0;
    t->render.height = 0;
    t->render.attached = false;
}
