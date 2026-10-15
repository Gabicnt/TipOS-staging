/* ovsb.term — term.h — contrato central (moe moe kyun <3)
 *
 * Define TODAS as structs e protótipos. Módulos incluem só "term.h".
 * Headers externos (wayland/fcft/xkb) NÃO entram aqui — só forward
 * declarations, pra testes unitários compilarem sem -I externos.
 */
#ifndef OVSB_TERM_H
#define OVSB_TERM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/* ~~ forward declarations (opacos aqui, reais nos .c) ~~ */
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_shm;
struct wl_shm_pool;
struct wl_buffer;
struct wl_surface;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct fcft_font;
struct fcft_glyph;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

/* ================= cursor ================= */
typedef enum {
    CURSOR_BLOCK = 0,
    CURSOR_UNDERLINE,
    CURSOR_BAR,
    CURSOR_HOLLOW
} cursor_style_t;

/* ================= grid.c ================= */
typedef struct {
    uint32_t codepoint;
    uint32_t fg;            /* ARGB */
    uint32_t bg;            /* ARGB */
    uint16_t attrs;
    uint8_t  width;         /* 1 ou 2 */
    uint8_t  _pad[3];
} cell_t;

#define ATTR_BOLD       (1u << 0)
#define ATTR_ITALIC     (1u << 1)
#define ATTR_UNDERLINE  (1u << 2)
#define ATTR_BLINK      (1u << 3)
#define ATTR_REVERSE    (1u << 4)
#define ATTR_HIDDEN     (1u << 5)
#define ATTR_STRIKE     (1u << 6)
#define ATTR_DIM        (1u << 7)
#define ATTR_OVERLINE   (1u << 8)
#define ATTR_WIDE       (1u << 9)
#define ATTR_WIDE_TAIL  (1u << 10)

typedef struct term term_t; /* fwd */

typedef struct {
    cell_t  *cells;
    int      rows, cols;
    int      cursor_x, cursor_y;
    int      saved_x, saved_y;
    bool     cursor_visible;
    cursor_style_t cursor_style;
    bool     cursor_blink;
    uint32_t cursor_color;
    uint32_t default_fg, default_bg;
    uint16_t cur_attrs;
    uint32_t cur_fg, cur_bg;
    uint32_t palette[256];
    int      scroll_top, scroll_bottom;
    bool     wrap_next;
    bool     origin_mode;
    bool     insert_mode;
    bool     auto_wrap;
    bool     bracketed_paste;
    bool     mouse_tracking;
    bool     app_cursor;        /* DECCKM */
    bool     app_keypad;        /* DECKPAM */
    uint8_t *dirty;
    int      dirty_count;
    uint32_t last_char;        /* p/ REP */
} grid_t;

void grid_init(term_t *t, int rows, int cols);
void grid_resize(term_t *t, int rows, int cols);
void grid_cleanup(term_t *t);
void grid_put_char(term_t *t, uint32_t cp, int width);
void grid_put_string(term_t *t, const char *utf8);
void grid_linefeed(term_t *t);
void grid_carriage_return(term_t *t);
void grid_tab(term_t *t);
void grid_backspace(term_t *t);
void grid_cursor_move(term_t *t, int dx, int dy);
void grid_cursor_set(term_t *t, int x, int y);
void grid_cursor_save(term_t *t);
void grid_cursor_restore(term_t *t);
void grid_erase_display(term_t *t, int mode);
void grid_erase_line(term_t *t, int mode);
void grid_erase_chars(term_t *t, int n);
void grid_insert_lines(term_t *t, int n);
void grid_delete_lines(term_t *t, int n);
void grid_insert_chars(term_t *t, int n);
void grid_delete_chars(term_t *t, int n);
void grid_scroll_up(term_t *t, int n);
void grid_scroll_down(term_t *t, int n);
void grid_set_fg(term_t *t, uint32_t color);
void grid_set_bg(term_t *t, uint32_t color);
void grid_reset_attrs(term_t *t);
void grid_mark_dirty(grid_t *g, int row);
void grid_mark_all_dirty(grid_t *g);
void grid_clear_dirty(grid_t *g);

/* ================= escape.c ================= */
typedef enum {
    PS_GROUND = 0,
    PS_ESC,
    PS_ESC_INT,
    PS_CSI_ENTRY,
    PS_CSI_PARAM,
    PS_CSI_INT,
    PS_CSI_IGNORE,
    PS_OSC_STRING,
    PS_DCS_ENTRY,
    PS_DCS_PASSTHRU,
    PS_DCS_IGNORE,
    PS_SOS_PM_APC
} parser_state_t;

typedef struct {
    parser_state_t state;
    int    params[16];
    int    nparams;
    bool   param_has_value[16];
    int    current_param;
    char   private_marker;   /* '?', '>', '=', '<', 0 */
    char   intermediate;
    char   osc_buf[256];
    int    osc_len;
    bool   osc_esc;          /* viu ESC dentro de OSC (ESC \) */
    char   dcs_buf[4096];
    int    dcs_len;
    uint32_t utf8_cp;
    int      utf8_remaining;
} parser_t;

void parser_init(parser_t *p);
void escape_feed(term_t *t, const char *buf, size_t len);

/* ================= scrollback.c ================= */
typedef struct {
    cell_t *lines;
    int     capacity;
    int     head;
    int     count;
    int     view_offset;
    int     cols;
} scrollback_t;

void scrollback_init(term_t *t, int capacity);
void scrollback_cleanup(term_t *t);
void scrollback_push(term_t *t, cell_t *line);
void scrollback_scroll(term_t *t, int delta);
void scrollback_clear(term_t *t);
cell_t *scrollback_get(scrollback_t *sb, int offset); /* 0 = mais antiga visível */

/* ================= selection.c / clipboard.c ================= */
typedef struct {
    int      start_x, start_y;
    int      end_x, end_y;
    bool     active;
    bool     rectangular;
    char    *text;
    size_t   text_len;
} selection_t;

typedef struct {
    char    *data;
    size_t   len;
    bool     is_set;
} clipboard_t;

void selection_start(term_t *t, int x, int y);
void selection_extend(term_t *t, int x, int y);
void selection_finish(term_t *t);
void selection_clear(term_t *t);
char *selection_get_text(term_t *t); /* malloc'd, caller free */
void clipboard_set(term_t *t, const char *text, size_t len);
void clipboard_paste(term_t *t);     /* escreve clipboard no pty */
void clipboard_wl_init(term_t *t);
void clipboard_wl_offer(term_t *t, const char *mime, int fd);
void clipboard_wl_receive(term_t *t, const char *mime, int fd);

/* ================= pty.c ================= */
typedef struct {
    int    master_fd;
    pid_t  child_pid;
    char   slave_path[64];
    bool   alive;
} pty_t;

bool    pty_init(term_t *t);
void    pty_cleanup(term_t *t);
ssize_t pty_read(term_t *t, char *buf, size_t len);
ssize_t pty_write(term_t *t, const char *buf, size_t len);
void    pty_resize(term_t *t, int cols, int rows);
void    pty_shell_exited(term_t *t);

/* ================= input.c ================= */
typedef struct {
    struct xkb_context *xkb_ctx;
    struct xkb_keymap  *xkb_keymap;
    struct xkb_state   *xkb_state;
    int     repeat_rate;
    int     repeat_delay;
    uint32_t mods_depressed;
    uint32_t mods_latched;
    uint32_t mods_locked;
    uint32_t mods_group;
    bool     mouse_tracking;
    uint32_t mouse_mode;
    int      mouse_x, mouse_y;
    uint32_t mouse_buttons;
    bool     selecting;
    int      sel_start_x, sel_start_y;
} input_t;

void input_init(term_t *t);
void input_cleanup(term_t *t);
void input_key(term_t *t, uint32_t keycode, uint32_t state); /* state: 0 release 1 press */
void input_modifiers(term_t *t, uint32_t dep, uint32_t latched,
                     uint32_t locked, uint32_t group);
void input_pointer_motion(term_t *t, int sx, int sy);
void input_pointer_button(term_t *t, uint32_t button, uint32_t state);
void input_pointer_axis(term_t *t, uint32_t axis, int value);

/* ================= font.c / glyph_cache.c ================= */
typedef struct {
    const struct fcft_glyph *glyph;
    uint32_t codepoint;
    uint32_t variant;      /* 0 reg, 1 bold, 2 italic, 3 bi */
    uint64_t last_used;
} glyph_entry_t;

typedef struct {
    glyph_entry_t *entries;
    int            capacity;
    int            count;
    uint64_t       clock;
} glyph_cache_t;

typedef struct {
    struct fcft_font *regular;
    struct fcft_font *bold;
    struct fcft_font *italic;
    struct fcft_font *bold_italic;
    bool     ready;        /* false = sem fcft (fallback bitmap) */
    int      cell_width;
    int      cell_height;
    int      ascent, descent;
    int      dpi;
    glyph_cache_t cache;
} font_t;

void font_init(term_t *t);
void font_cleanup(term_t *t);
void font_resize(term_t *t, int pixel_size);
const struct fcft_glyph *font_glyph(term_t *t, uint32_t cp, uint16_t attrs);
int  font_cell_width(font_t *f);
int  font_cell_height(font_t *f);
void glyph_cache_init(glyph_cache_t *c, int capacity);
void glyph_cache_done(glyph_cache_t *c);

/* ================= shm.c / render.c / cursor.c ================= */
typedef struct {
    struct wl_shm_pool *pool;
    struct wl_buffer   *buffer;
    uint32_t *pixels;
    size_t    size;
    int       fd;
    int       width, height, stride;
    bool      busy;
    bool      valid;
} shm_buffer_t;

typedef struct {
    shm_buffer_t buffers[2];
    int          current;
    int          width, height;   /* pixels */
    bool         attached;
} render_t;

shm_buffer_t *shm_buffer_create(struct wl_shm *shm, int width, int height);
void shm_buffer_destroy(shm_buffer_t *buf);

void render_init(term_t *t);
void render_resize(term_t *t, int width, int height);
void render_update(term_t *t);
void render_full(term_t *t);
void render_cursor(term_t *t);
void render_cleanup(term_t *t);
void cursor_draw(term_t *t, uint32_t *pixels, int stride,
                 int cell_x, int cell_y); /* desenha cursor no shm */

/* ================= timer.c / signal.c ================= */
int  timer_create_ms(int ms, bool periodic);
void timer_disarm(int fd);
void timer_set(int fd, int ms, bool periodic);

void signal_init(term_t *t);
void signal_cleanup(term_t *t);

/* ================= loop.c ================= */
typedef struct {
    int  poll_fds_extra;  /* reservado */
    int  wl_fd;
    int  pty_fd;
    int  signal_pipe[2];
    int  timer_fd;
    bool blink_on;
} loop_t;

void loop_init(term_t *t);
void loop_run(term_t *t);
void loop_cleanup(term_t *t);
void loop_handle_pty(term_t *t);
void loop_handle_timer(term_t *t);

/* ================= wl_client.c ================= */
typedef struct {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct wl_seat       *seat;
    struct xdg_wm_base   *xdg_wm_base;
    struct wl_keyboard   *keyboard;
    struct wl_pointer    *pointer;
    struct wl_surface    *surface;
    struct xdg_surface   *xdg_surface;
    struct xdg_toplevel  *xdg_toplevel;
    int   width, height;      /* pixels */
    int   scale;
    bool  configured;
    bool  have_display;
} wl_state_t;

bool wl_init(term_t *t);
void wl_cleanup(term_t *t);
bool wl_attach(term_t *t); /* attach+commit do buffer corrente */

/* ================= config.c ================= */
typedef struct {
    char     font_family[128];
    int      font_size;
    int      dpi;
    uint32_t bg, fg, cursor;
    uint32_t sel_bg, sel_fg;
    uint32_t palette[16];
    cursor_style_t cursor_style;
    bool     cursor_blink;
    int      cursor_blink_interval;
    int      scrollback_lines;
    bool     alt_sends_escape;
    bool     shift_pgup_scroll;
    bool     mouse_tracking;
    char     shell[128];
    char     term_name[32];
} config_t;

void config_defaults(config_t *c);
void config_load(config_t *c, const char *path);
void config_reload(term_t *t);

/* ================= log.c / util.c ================= */
void log_init(bool verbose);
void log_msg(const char *fmt, ...);
void log_err(const char *fmt, ...);

uint32_t util_parse_color(const char *s);      /* "#rrggbb" -> ARGB */
int      util_wcwidth(uint32_t cp);            /* 1 ou 2 */
size_t   util_utf8_encode(char *out, uint32_t cp);
int      util_utf8_decode(const char *s, size_t n, uint32_t *cp);
void     util_base64_encode(const char *in, size_t n, char *out, size_t outsz);

/* ================= term_t ================= */
struct term {
    wl_state_t   wl;
    grid_t       grid;
    parser_t     parser;
    scrollback_t scrollback;
    selection_t  selection;
    clipboard_t  clipboard;
    font_t       font;
    render_t     render;
    pty_t        pty;
    input_t      input;
    loop_t       loop;
    config_t     config;
    char         title[128];
    char         config_path[256];
    bool         running;
    int          exit_code;
    bool         verbose;
};

void term_cleanup(term_t *t);

#endif /* OVSB_TERM_H */
