# `ovsb.term` — Arquitetura Completa de um Terminal Wayland Nativo

**Versão:** 1.0 (especificação total, sem cortes)
**Kernel alvo:** OvsbMk (x86_64, compat Linux ELF, framebuffer VESA)
**Compositor:** `WAYLAND_COMPOSITOR_LIB` (libwayland-server + xdg_shell)
**Linguagem:** C11 (freestanding, sem libc do host)
**Binário final:** static, ~5-8 MB stripped

---

## PARTE I — VISÃO GERAL

### 1. Princípio de Design

O `ovsb.term` é um **terminal Wayland nativo**, escrito em C puro, que:

1. **Conecta** ao compositor via `libwayland-client`.
2. **Cria uma janela** via `xdg_shell`.
3. **Roda um shell** dentro de um PTY.
4. **Renderiza** o output do shell num **buffer de células** (`grid`).
5. **Desenha** o grid no framebuffer via `wl_shm` (memória compartilhada).
6. **Traduz** input do teclado (XKB) em bytes pro PTY.
7. **Reusa** as libs já portadas (`fcft`, `pixman`, `harfbuzz`, `xkbcommon`, `utf8proc`).

**Filosofia:** máximo de compatibilidade com o ecossistema Unix (terminfo, VT100, xterm), usando a pilha Wayland moderna.

### 2. Não-Objetivos (mas com plano)

**Nada é cortado por "ser difícil".** Cada item abaixo tem arquitetura, mesmo que seja pra v2:

- **`terminfo` completo** → implementado (parser + entrada própria).
- **Unicode complexo** (emoji ZWJ, Bidi, indic) → implementado via `harfbuzz` + `fcft`.
- **Ligatures** → implementado via `harfbuzz`.
- **Mouse tracking** (X10, VT200, SGR) → implementado.
- **Bracketed paste** → implementado.
- **Sixel graphics** → implementado (v2).
- **Kitty graphics protocol** → implementado (v2).
- **iTerm2 inline images** → implementado (v2).
- **Abas** → implementado.
- **Split** → implementado.
- **`wl_data_device`** (clipboard Wayland) → implementado.
- **HiDPI** → implementado.
- **Aceleração GPU** → fora de escopo (OvsbMk não tem GPU).
- **`tmux`/`vim`/`htop`** → devem funcionar (via `terminfo` completo).

---

## PARTE II — ARQUITETURA DE COMPONENTES

### 3. Mapa de Módulos

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          ovsb.term (processo ring 3)                     │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │                        APPLICATION LAYER                           │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐          │  │
│  │  │  main.c  │  │ config.c │  │  log.c   │  │  util.c  │          │  │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘          │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                    │                                     │
│  ┌────────────────────────────────▼────────────────────────────────────┐ │
│  │                       ORCHESTRATION LAYER                          │ │
│  │  ┌──────────────────────────────────────────────────────────────┐  │ │
│  │  │                    loop.c  (event loop)                      │  │ │
│  │  │  epoll: wayland_fd, pty_fd, signal_fd, timer_fd, inotify_fd  │  │ │
│  │  └──────────────────────────────────────────────────────────────┘  │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                     │
│  ┌────────────────────────────────▼────────────────────────────────────┐ │
│  │                        PROTOCOL LAYER                              │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐          │ │
│  │  │ wl_      │  │ escape.c │  │  input.c │  │  pty.c   │          │ │
│  │  │ client.c │  │ (parser) │  │  (kbd)   │  │  (shell) │          │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘          │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                     │
│  ┌────────────────────────────────▼────────────────────────────────────┐ │
│  │                          MODEL LAYER                               │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐          │ │
│  │  │ grid.c   │  │scrollback│  │selection │  │  undo.c  │          │ │
│  │  │ (cells)  │  │  .c      │  │  .c      │  │          │          │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘          │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                     │
│  ┌────────────────────────────────▼────────────────────────────────────┐ │
│  │                         RENDER LAYER                               │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐          │ │
│  │  │ font.c   │  │ render.c │  │ glyph_   │  │  cursor  │          │ │
│  │  │ (fcft)   │  │ (pixman) │  │ cache.c  │  │  .c      │          │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘          │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                     │
│  ┌────────────────────────────────▼────────────────────────────────────┐ │
│  │                        PLATFORM LAYER                              │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐          │ │
│  │  │  shm.c   │  │  timer   │  │  signal  │  │ clipboard│          │ │
│  │  │ (memfd)  │  │  .c      │  │  .c      │  │  .c      │          │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘          │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
            ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
            │  Wayland    │ │    PTY      │ │   Kernel    │
            │  Compositor │ │ /dev/ptmx   │ │  (syscalls) │
            └─────────────┘ └─────────────┘ └─────────────┘
```

### 4. Fluxo de Dados (visão macro)

```
┌──────────┐   write()   ┌──────────┐   read()   ┌──────────┐
│  Shell   │────────────►│   PTY    │───────────►│ pty.c    │
│ (/bin/sh)│             │  master  │            │          │
└──────────┘             └──────────┘            └────┬─────┘
     ▲                                                 │
     │                                                 │ bytes
     │ write()                                         ▼
     │                                           ┌──────────┐
     │                                           │escape.c  │
     │                                           │(parser)  │
     │                                           └────┬─────┘
     │                                                │ calls
     │                                                ▼
     │                                           ┌──────────┐
     │                                           │ grid.c   │
     │                                           │ (cells)  │
     │                                           └────┬─────┘
     │                                                │ dirty
     │                                                ▼
     │                                           ┌──────────┐
     │                                           │render.c  │
     │                                           │(pixman)  │
     │                                           └────┬─────┘
     │                                                │ pixels
     │                                                ▼
     │                                           ┌──────────┐
     │                                           │  shm.c   │
     │                                           │ (memfd)  │
     │                                           └────┬─────┘
     │                                                │
     │                                                │ wl_buffer
     │                                                ▼
     │                                           ┌──────────┐
     │                                           │ Wayland  │
     │                                           │compositor│
     │                                           └──────────┘
     │
     │ write()
     │
┌────┴─────┐   key event  ┌──────────┐  keysym   ┌──────────┐
│ input.c  │◄─────────────│wl_client │◄──────────│Compositor│
│ (XKB)    │              │  .c      │           │          │
└──────────┘              └──────────┘           └──────────┘
```

---

## PARTE III — MÓDULOS DETALHADOS

### 5. `main.c` — Ponto de entrada

**Responsabilidade:** inicializar tudo, rodar o loop, limpar ao sair.

**Estrutura:**
```c
typedef struct {
    // Wayland
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

    // Modelo
    grid_t       grid;
    scrollback_t scrollback;
    cursor_t     cursor;

    // Render
    font_t       font;
    render_t     render;

    // PTY
    pty_t        pty;

    // Input
    input_t      input;

    // Config
    config_t     config;

    // Estado
    bool         running;
    int          exit_code;
} term_t;

int main(int argc, char **argv) {
    term_t term = {0};
    config_load(&term.config, "::/ETC/OVSB.TERM.CONF");
    wl_init(&term);
    font_init(&term);
    grid_init(&term, 80, 24);
    scrollback_init(&term, 1000);
    render_init(&term);
    pty_init(&term);
    input_init(&term);
    loop_run(&term);
    term_cleanup(&term);
    return term.exit_code;
}
```

### 6. `wl_client.c` — Wayland Client

**Responsabilidade:** conexão com o compositor, criar superfície, processar eventos.

**Estrutura:**
```c
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
    int                   width, height;
    int                   scale;        // HiDPI
    bool                  configured;
} wl_state_t;
```

**Funções:**
```c
void wl_init(term_t *t);
void wl_cleanup(term_t *t);

// Registry
static void registry_global(void *data, struct wl_registry *r,
                            uint32_t name, const char *iface, uint32_t ver);
static void registry_global_remove(void *data, struct wl_registry *r,
                                   uint32_t name);

// Surface
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial);
static void xdg_surface_configure(void *data, struct xdg_surface *s,
                                  uint32_t serial);
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl,
                                   int32_t w, int32_t h, struct wl_array *states);
static void xdg_toplevel_close(void *data, struct xdg_toplevel *tl);

// Input
static void keyboard_keymap(void *data, struct wl_keyboard *kb,
                            uint32_t format, int32_t fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *kb,
                           uint32_t serial, struct wl_surface *s,
                           struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *kb,
                           uint32_t serial, struct wl_surface *s);
static void keyboard_key(void *data, struct wl_keyboard *kb,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                               uint32_t serial, uint32_t dep,
                               uint32_t latched, uint32_t locked, uint32_t group);
static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                                 int32_t rate, int32_t delay);

// Pointer
static void pointer_enter(void *data, struct wl_pointer *p,
                          uint32_t serial, struct wl_surface *s,
                          wl_fixed_t sx, wl_fixed_t sy);
static void pointer_leave(void *data, struct wl_pointer *p,
                          uint32_t serial, struct wl_surface *s);
static void pointer_motion(void *data, struct wl_pointer *p,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy);
static void pointer_button(void *data, struct wl_pointer *p,
                           uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state);
static void pointer_axis(void *data, struct wl_pointer *p,
                         uint32_t time, uint32_t axis, wl_fixed_t value);
static void pointer_frame(void *data, struct wl_pointer *p);
```

### 7. `grid.c` — Modelo do Terminal

**Responsabilidade:** buffer de células, cursor, dirty regions.

**Estrutura:**
```c
typedef struct {
    uint32_t codepoint;    // Unicode codepoint
    uint32_t fg;           // ARGB foreground
    uint32_t bg;           // ARGB background
    uint16_t attrs;        // atributos (bitmask)
    uint8_t  width;        // 1 ou 2 (CJK/emoji)
    uint8_t  _pad[3];
} cell_t;

// Atributos (bitmask)
#define ATTR_BOLD       (1 << 0)
#define ATTR_ITALIC     (1 << 1)
#define ATTR_UNDERLINE  (1 << 2)
#define ATTR_BLINK      (1 << 3)
#define ATTR_REVERSE    (1 << 4)
#define ATTR_HIDDEN     (1 << 5)
#define ATTR_STRIKE     (1 << 6)
#define ATTR_DIM        (1 << 7)
#define ATTR_OVERLINE   (1 << 8)
#define ATTR_WIDE       (1 << 9)
#define ATTR_WIDE_TAIL  (1 << 10)   // segunda célula de CJK

typedef struct {
    cell_t  *cells;             // rows * cols
    int      rows, cols;        // dimensões atuais
    int      cursor_x, cursor_y;
    bool     cursor_visible;
    cursor_style_t cursor_style;
    bool     cursor_blink;
    uint32_t cursor_color;
    uint32_t default_fg, default_bg;
    uint16_t default_attrs;
    uint32_t *palette;          // 256 cores
    int      scroll_top, scroll_bottom; // região de scroll
    bool     wrap_next;         // próximo char wrap?
    bool     origin_mode;       // DECOM
    bool     insert_mode;       // IRM
    bool     auto_wrap;         // DECAWM
    bool     bracketed_paste;
    bool     mouse_tracking;
    uint8_t  *dirty;            // 1 bit por linha
    int      dirty_count;
} grid_t;
```

**Funções:**
```c
void grid_init(term_t *t, int rows, int cols);
void grid_resize(term_t *t, int rows, int cols);
void grid_cleanup(term_t *t);

// Escrita
void grid_put_char(term_t *t, uint32_t cp, int width);
void grid_put_string(term_t *t, const char *utf8);

// Cursor
void grid_cursor_move(term_t *t, int dx, int dy);
void grid_cursor_set(term_t *t, int x, int y);
void grid_cursor_save(term_t *t);
void grid_cursor_restore(term_t *t);

// Edição
void grid_erase_display(term_t *t, int mode);
void grid_erase_line(term_t *t, int mode);
void grid_insert_lines(term_t *t, int n);
void grid_delete_lines(term_t *t, int n);
void grid_insert_chars(term_t *t, int n);
void grid_delete_chars(term_t *t, int n);
void grid_scroll_up(term_t *t, int n);
void grid_scroll_down(term_t *t, int n);

// Atributos
void grid_set_attrs(term_t *t, uint16_t attrs);
void grid_set_fg(term_t *t, uint32_t color);
void grid_set_bg(term_t *t, uint32_t color);
void grid_reset_attrs(term_t *t);

// Dirty
void grid_mark_dirty(grid_t *g, int row);
void grid_mark_all_dirty(grid_t *g);
void grid_clear_dirty(grid_t *g);
```

### 8. `escape.c` — Parser de Sequências de Escape

**Responsabilidade:** máquina de estados que interpreta VT100/xterm.

**Estrutura:**
```c
typedef enum {
    PS_GROUND,      // texto normal
    PS_ESC,         // recebeu ESC
    PS_ESC_INT,     // ESC ( ) # etc
    PS_CSI_ENTRY,   // ESC [
    PS_CSI_PARAM,   // ESC [ 1;2
    PS_CSI_INT,     // ESC [ 1;2;
    PS_CSI_IGNORE,  // ESC [ > (ignora)
    PS_OSC_STRING,  // ESC ]
    PS_DCS_ENTRY,   // ESC P
    PS_DCS_PASSTHRU,
    PS_DCS_IGNORE,
    PS_SOS_PM_APC,  // ESC X ^ _
} parser_state_t;

typedef struct {
    parser_state_t state;
    int    params[16];       // parâmetros numéricos
    int    nparams;
    bool   param_has_value[16];
    int    current_param;
    bool   private_marker;   // '?' '>' '<' '='
    char   intermediate;     // ' ', '!', '"', etc.
    // OSC
    char   osc_buf[256];
    int    osc_len;
    // DCS
    char   dcs_buf[4096];
    int    dcs_len;
    // UTF-8
    uint32_t utf8_cp;
    int      utf8_remaining;
} parser_t;
```

**Tabela de sequências suportadas (completa):**

**Controle (C0):**
| Código | Nome | Ação |
|---|---|---|
| 0x07 | BEL | Bell |
| 0x08 | BS | Backspace |
| 0x09 | HT | Tab |
| 0x0A | LF | Line feed |
| 0x0B | VT | Vertical tab |
| 0x0C | FF | Form feed |
| 0x0D | CR | Carriage return |
| 0x1B | ESC | Escape |
| 0x7F | DEL | Delete |

**CSI (ESC [):**
| Sequência | Nome | Ação |
|---|---|---|
| `CSI n A` | CUU | Cursor up |
| `CSI n B` | CUD | Cursor down |
| `CSI n C` | CUF | Cursor forward |
| `CSI n D` | CUB | Cursor back |
| `CSI n E` | CNL | Cursor next line |
| `CSI n F` | CPL | Cursor prev line |
| `CSI n G` | CHA | Cursor horizontal absolute |
| `CSI y ; x H` | CUP | Cursor position |
| `CSI y ; x f` | HVP | Horizontal & vertical position |
| `CSI n J` | ED | Erase display (0=below, 1=above, 2=all, 3=scrollback) |
| `CSI n K` | EL | Erase line (0=right, 1=left, 2=all) |
| `CSI n L` | IL | Insert lines |
| `CSI n M` | DL | Delete lines |
| `CSI n P` | DCH | Delete chars |
| `CSI n @` | ICH | Insert chars |
| `CSI n S` | SU | Scroll up |
| `CSI n T` | SD | Scroll down |
| `CSI n X` | ECH | Erase chars |
| `CSI n Z` | CBT | Cursor back tab |
| `CSI n b` | REP | Repeat last char |
| `CSI n c` | DA | Device attributes |
| `CSI n d` | VPA | Vertical position absolute |
| `CSI n e` | VPR | Vertical position relative |
| `CSI n g` | TBC | Tab clear |
| `CSI n h` | SM | Set mode |
| `CSI n l` | RM | Reset mode |
| `CSI n m` | SGR | Select Graphic Rendition |
| `CSI n q` | DECSCUSR | Cursor style |
| `CSI n r` | DECSTBM | Set scroll region |
| `CSI n s` | SCOSC | Save cursor |
| `CSI n u` | SCORC | Restore cursor |
| `CSI n t` | Window ops | (título, resize) |
| `CSI ? n h` | DECSET | Set private mode |
| `CSI ? n l` | DECRST | Reset private mode |
| `CSI ? n J` | DECSED | Selective erase display |
| `CSI ? n K` | DECSEL | Selective erase line |
| `CSI > n c` | DA2 | Secondary DA |
| `CSI = n c` | DA3 | Tertiary DA |
| `CSI n * x` | DECSACE | etc |

**SGR (cores/atributos):**
| Sequência | Ação |
|---|---|
| `0` | Reset |
| `1` | Bold |
| `2` | Dim |
| `3` | Italic |
| `4` | Underline |
| `4:0` a `4:5` | Underline style (curly, dotted, etc.) |
| `5` | Blink slow |
| `6` | Blink rapid |
| `7` | Reverse |
| `8` | Hidden |
| `9` | Strikethrough |
| `21` | Double underline |
| `22` | Normal intensity |
| `23` | Not italic |
| `24` | Not underline |
| `25` | Not blink |
| `27` | Not reverse |
| `28` | Not hidden |
| `29` | Not strike |
| `30-37` | FG (8 cores) |
| `38;5;n` | FG 256 cores |
| `38;2;r;g;b` | FG TrueColor |
| `39` | FG default |
| `40-47` | BG (8 cores) |
| `48;5;n` | BG 256 cores |
| `48;2;r;g;b` | BG TrueColor |
| `49` | BG default |
| `53` | Overline |
| `55` | Not overline |
| `90-97` | FG bright |
| `100-107` | BG bright |

**OSC (ESC ]):**
| Sequência | Ação |
|---|---|
| `0;title BEL` | Set title + icon |
| `1;title BEL` | Set icon |
| `2;title BEL` | Set title |
| `4;n;color BEL` | Set palette color |
| `7;file BEL` | Set working directory |
| `8;params;uri BEL` | Hyperlink |
| `9;n;file BEL` | Growl notify |
| `10;color BEL` | Set FG |
| `11;color BEL` | Set BG |
| `12;color BEL` | Set cursor |
| `52;c;base64 BEL` | Clipboard |
| `104;n BEL` | Reset palette |
| `110 BEL` | Reset FG |
| `111 BEL` | Reset BG |
| `112 BEL` | Reset cursor |
| `133;... BEL` | Shell integration (iTerm2) |

**DCS (ESC P):**
| Sequência | Ação |
|---|---|
| `DCS $q... ST` | DECRQSS (request status) |
| `DCS +q... ST` | XTGETTCAP |
| `DCS q... ST` | Sixel graphics |
| `DCS G... ST` | Kitty graphics |

**ESC (fora de CSI):**
| Sequência | Ação |
|---|---|
| `ESC 7` | DECSC (save cursor) |
| `ESC 8` | DECRC (restore cursor) |
| `ESC =` | DECKPAM (keypad app) |
| `ESC >` | DECKPNM (keypad numeric) |
| `ESC D` | IND (index) |
| `ESC E` | NEL (next line) |
| `ESC H` | HTS (set tab stop) |
| `ESC M` | RI (reverse index) |
| `ESC N` | SS2 (single shift 2) |
| `ESC O` | SS3 (single shift 3) |
| `ESC P` | DCS |
| `ESC [ ` | CSI |
| `ESC \ ` | ST (string terminator) |
| `ESC ] ` | OSC |
| `ESC X` | SOS |
| `ESC ^` | PM |
| `ESC _` | APC |
| `ESC c` | RIS (full reset) |

**UTF-8 decoding:**
```c
// Bytes UTF-8:
// 0xxxxxxx                                          → 1 byte  (0x00-0x7F)
// 110xxxxx 10xxxxxx                                 → 2 bytes (0x80-0x7FF)
// 1110xxxx 10xxxxxx 10xxxxxx                        → 3 bytes (0x800-0xFFFF)
// 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx               → 4 bytes (0x10000-0x10FFFF)
```

### 9. `render.c` — Render Engine

**Responsabilidade:** compor o grid no buffer Wayland.

**Estrutura:**
```c
typedef struct {
    struct wl_shm_pool *pool;
    struct wl_buffer   *buffer;
    uint32_t           *pixels;
    size_t              size;
    int                 width, height, stride;
    bool                busy;
} shm_buffer_t;

typedef struct {
    shm_buffer_t    buffers[2];         // double buffer
    int             current;
    shm_buffer_t   *pending;
    pixman_image_t *bg_image;           // para composição
    pixman_image_t *fg_image;
    uint32_t       *scratch;            // buffer temporário
    int             width, height;
} render_t;
```

**Funções:**
```c
void render_init(term_t *t);
void render_resize(term_t *t, int width, int height);
void render_update(term_t *t);          // desenha dirty regions
void render_full(term_t *t);            // desenha tudo
void render_cursor(term_t *t);          // desenha cursor
void render_cleanup(term_t *t);

// Internos
static void render_cell(render_t *r, cell_t *c, int x, int y);
static void render_glyph(render_t *r, const struct fcft_glyph *g,
                         int x, int y, uint32_t fg, uint32_t bg);
static void render_bg(render_t *r, int x, int y, int w, int h, uint32_t color);
static shm_buffer_t *render_alloc_buffer(render_t *r, int w, int h);
```

**Pipeline de renderização:**
1. **Dirty check** — se `dirty_count == 0`, retorna.
2. **Seleciona buffer** (double buffer, o que não está `busy`).
3. **Para cada linha dirty:**
   - Para cada célula:
     - **Fundo:** preenche `cell->bg` no retângulo.
     - **Glifo:** pega `fcft_rasterize_char_utf32`, compõe com `pixman`.
     - **Atributos:** aplica (bold = dobra a espessura, underline = linha, reverse = swap fg/bg).
     - **Cursor:** se for a célula do cursor, desenha o cursor.
4. **`wl_surface.attach(buffer)` + `wl_surface.commit`.**
5. **Marca `busy`** — espera o evento `wl_buffer.release` pra reusar.

### 10. `font.c` — Font Manager

**Responsabilidade:** carregar fonte, rasterizar glifos, cachear.

**Estrutura:**
```c
typedef struct {
    struct fcft_font *regular;
    struct fcft_font *bold;
    struct fcft_font *italic;
    struct fcft_font *bold_italic;
    int               cell_width;
    int               cell_height;
    int               ascent, descent;
    int               line_gap;
    float             dpi;
    glyph_cache_t     cache;
} font_t;

typedef struct {
    struct fcft_glyph *glyph;
    uint32_t           codepoint;
    uint32_t           attrs;       // bold/italic
    uint64_t           last_used;   // LRU
} glyph_entry_t;

typedef struct {
    glyph_entry_t *entries;
    int            capacity;    // ex: 512
    int            count;
    uint64_t       clock;
} glyph_cache_t;
```

**Funções:**
```c
void font_init(term_t *t);
void font_cleanup(term_t *t);
void font_resize(term_t *t, int pixel_size);

const struct fcft_glyph *font_glyph(font_t *f, uint32_t cp, uint16_t attrs);
int font_cell_width(font_t *f);
int font_cell_height(font_t *f);
```

**Uso do `fcft`:**
- `fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_ERROR)`
- `fcft_from_name(1, attrs, NULL)` — `attrs = {"monospace:size=12", NULL}`
- `fcft_rasterize_char_utf32(font, cp, FCFT_SUBPIXEL_NONE)`
- `fcft_rasterize_grapheme_utf32(font, cps, n, FCFT_SUBPIXEL_NONE)` — grapheme clusters
- `fcft_kerning(font, left, right)`
- `fcft_precompose(font, base, comb, &cp, &n)`

### 11. `input.c` — Input Handler

**Responsabilidade:** traduzir eventos Wayland em bytes pro PTY.

**Estrutura:**
```c
typedef struct {
    struct xkb_context *xkb_ctx;
    struct xkb_keymap  *xkb_keymap;
    struct xkb_state   *xkb_state;
    int                 keymap_fd;
    int                 repeat_rate;
    int                 repeat_delay;
    int                 pty_fd;

    // Modifiers
    uint32_t mods_depressed;
    uint32_t mods_latched;
    uint32_t mods_locked;
    uint32_t mods_group;

    // Mouse
    bool     mouse_tracking;
    uint32_t mouse_mode;        // X10, VT200, SGR, etc.
    int      mouse_x, mouse_y;
    uint32_t mouse_buttons;

    // Selection
    bool     selecting;
    int      sel_start_x, sel_start_y;
    int      sel_end_x, sel_end_y;

    // Key repeat
    int      repeat_timer_fd;
    uint32_t last_key;
} input_t;
```

**Funções:**
```c
void input_init(term_t *t);
void input_cleanup(term_t *t);

// Teclado
void input_key(term_t *t, uint32_t keycode, uint32_t state);
void input_modifiers(term_t *t, uint32_t dep, uint32_t lat,
                     uint32_t locked, uint32_t group);

// Mouse
void input_pointer_motion(term_t *t, wl_fixed_t sx, wl_fixed_t sy);
void input_pointer_button(term_t *t, uint32_t button, uint32_t state);
void input_pointer_axis(term_t *t, uint32_t axis, wl_fixed_t value);

// Tradução
static void input_key_to_pty(term_t *t, uint32_t keycode, uint32_t state);
static void input_mouse_to_pty(term_t *t, uint32_t button, uint32_t state);
static void input_send_mouse(term_t *t, int button, int state, int x, int y);
```

**Tradução de tecla:**
1. `xkb_state_key_get_one_sym(state, keycode + 8)` → keysym.
2. `xkb_state_key_get_utf8(state, keycode + 8, buf, size)` → UTF-8.
3. Se `keysym` é **tecla de função** (F1-F12, setas, etc.):
   - Envia sequência de escape correspondente.
   - Ex: `XKB_KEY_Up` → `\x1b[A`.
4. Se `keysym` é **letra**:
   - Aplica **modifiers** (Ctrl, Alt, Shift).
   - Ctrl+A → `\x01`, Alt+A → `\x1ba`.

**Tabela de escape (teclas especiais):**
| Tecla | Sequência |
|---|---|
| Up | `\x1b[A` (normal) / `\x1bOA` (app mode) |
| Down | `\x1b[B` |
| Right | `\x1b[C` |
| Left | `\x1b[D` |
| Home | `\x1b[H` / `\x1b[1~` |
| End | `\x1b[F` / `\x1b[4~` |
| Page Up | `\x1b[5~` |
| Page Down | `\x1b[6~` |
| Insert | `\x1b[2~` |
| Delete | `\x1b[3~` |
| F1-F4 | `\x1bOP`-`\x1bOS` |
| F5-F12 | `\x1b[15~`-`\x1b[24~` |

### 12. `pty.c` — PTY Manager

**Responsabilidade:** abrir PTY, fork, execve shell, resize.

**Estrutura:**
```c
typedef struct {
    int    master_fd;
    int    slave_fd;
    pid_t  child_pid;
    char   slave_path[64];
    bool   alive;
    struct termios orig_termios;
} pty_t;
```

**Funções:**
```c
bool pty_init(term_t *t);
void pty_cleanup(term_t *t);
ssize_t pty_read(term_t *t, char *buf, size_t len);
ssize_t pty_write(term_t *t, const char *buf, size_t len);
void pty_resize(term_t *t, int cols, int rows);
void pty_set_termios(term_t *t);
void pty_restore_termios(term_t *t);
```

**Configuração do `termios`:**
```c
struct termios tio;
tcgetattr(slave, &tio);
tio.c_iflag = ICRNL | IXON;
tio.c_oflag = OPOST | ONLCR;
tio.c_cflag = CREAD | CS8 | HUPCL;
tio.c_lflag = ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE |
              ISIG | IEXTEN;
tio.c_cc[VINTR]    = 0x03;  // Ctrl+C
tio.c_cc[VQUIT]    = 0x1C;  // Ctrl+\
tio.c_cc[VERASE]   = 0x7F;  // DEL
tio.c_cc[VKILL]    = 0x15;  // Ctrl+U
tio.c_cc[VEOF]     = 0x04;  // Ctrl+D
tio.c_cc[VTIME]    = 0;
tio.c_cc[VMIN]     = 1;
tio.c_cc[VSTART]   = 0x11;  // Ctrl+Q
tio.c_cc[VSTOP]    = 0x13;  // Ctrl+S
tio.c_cc[VSUSP]    = 0x1A;  // Ctrl+Z
tio.c_cc[VREPRINT] = 0x12;  // Ctrl+R
tio.c_cc[VDISCARD] = 0x0F;  // Ctrl+O
tio.c_cc[VWERASE]  = 0x17;  // Ctrl+W
tio.c_cc[VLNEXT]   = 0x16;  // Ctrl+V
tcsetattr(slave, TCSANOW, &tio);
```

### 13. `loop.c` — Event Loop

**Responsabilidade:** orquestrar fds via `epoll`.

**Estrutura:**
```c
typedef struct {
    int              epoll_fd;
    struct epoll_event events[16];
    int              wl_fd;
    int              pty_fd;
    int              signal_pipe[2];
    int              timer_fd;
    int              inotify_fd;      // pra recarregar config
} loop_t;
```

**Funções:**
```c
void loop_init(term_t *t);
void loop_run(term_t *t);
void loop_cleanup(term_t *t);

static void loop_handle_wayland(term_t *t);
static void loop_handle_pty(term_t *t);
static void loop_handle_signal(term_t *t);
static void loop_handle_timer(term_t *t);
```

**Fluxo detalhado:**
```c
void loop_run(term_t *t) {
    while (t->running) {
        wl_display_dispatch_pending(t->wl.display);

        if (wl_display_flush(t->wl.display) == -1 && errno != EAGAIN)
            break;

        int n = epoll_wait(t->loop.epoll_fd, t->loop.events, 16, -1);
        for (int i = 0; i < n; i++) {
            int fd = t->loop.events[i].data.fd;
            if (fd == t->loop.wl_fd) {
                if (wl_display_dispatch(t->wl.display) == -1)
                    t->running = false;
            } else if (fd == t->loop.pty_fd) {
                char buf[4096];
                ssize_t r = read(fd, buf, sizeof(buf));
                if (r > 0) {
                    escape_feed(&t->parser, buf, r);
                    if (t->grid.dirty_count > 0)
                        render_update(t);
                } else if (r == 0 || (r < 0 && errno != EAGAIN)) {
                    pty_shell_exited(t);
                }
            } else if (fd == t->loop.signal_pipe[0]) {
                loop_handle_signal(t);
            } else if (fd == t->loop.timer_fd) {
                loop_handle_timer(t);
            } else if (fd == t->loop.inotify_fd) {
                config_reload(&t->config);
            }
        }
    }
}
```

### 14. `scrollback.c` — Histórico

**Responsabilidade:** ring buffer de linhas que saíram da tela.

**Estrutura:**
```c
typedef struct {
    cell_t *lines;          // ring buffer (capacity * cols)
    int     capacity;       // máximo de linhas
    int     head;           // onde escrever
    int     count;          // quantas linhas tem
    int     view_offset;    // 0 = bottom, capacity = top
    int     cols;
} scrollback_t;
```

**Funções:**
```c
void scrollback_init(term_t *t, int capacity);
void scrollback_cleanup(term_t *t);
void scrollback_push(term_t *t, cell_t *line);
void scrollback_scroll(term_t *t, int delta);
void scrollback_clear(term_t *t);
cell_t *scrollback_get(scrollback_t *sb, int offset);
```

### 15. `selection.c` — Seleção e Clipboard

**Responsabilidade:** selecionar texto e copiar/colar.

**Estrutura:**
```c
typedef struct {
    int      start_x, start_y;
    int      end_x, end_y;
    bool     active;
    bool     rectangular;   // Alt+drag = seleção retangular
    char    *text;          // texto extraído
    size_t   text_len;
} selection_t;

typedef struct {
    char    *data;
    size_t   len;
    bool     is_set;
} clipboard_t;
```

**Funções:**
```c
void selection_start(term_t *t, int x, int y);
void selection_extend(term_t *t, int x, int y);
void selection_finish(term_t *t);
void selection_clear(term_t *t);
char *selection_get_text(term_t *t);

void clipboard_set(term_t *t, const char *text);
void clipboard_paste(term_t *t);
```

### 16. `shm.c` — Buffer Wayland

**Responsabilidade:** criar `wl_shm_pool` + `wl_buffer` via `memfd`.

**Funções:**
```c
shm_buffer_t *shm_buffer_create(struct wl_shm *shm, int width, int height);
void shm_buffer_destroy(shm_buffer_t *buf);
void shm_buffer_attach(shm_buffer_t *buf, struct wl_surface *surface,
                       int x, int y);
```

**Como funciona:**
```c
int fd = memfd_create("ovsb-term-shm", MFD_CLOEXEC);
ftruncate(fd, size);
uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
struct wl_buffer *buffer = wl_shm_pool_create_buffer(
    pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
```

### 17. `timer.c` — Timerfd

**Responsabilidade:** cursor blink, refresh.

**Funções:**
```c
int timer_create_ms(int ms);
void timer_disarm(int fd);
void timer_set(int fd, int ms, bool periodic);
```

### 18. `signal.c` — Sinais

**Responsabilidade:** tratar `SIGCHLD`, `SIGWINCH`, `SIGTERM`.

**Estrutura:**
```c
// self-pipe pattern
int signal_pipe[2];
```

**Funções:**
```c
void signal_init(void);
void signal_cleanup(void);
static void signal_handler(int sig);
```

### 19. `clipboard.c` — Clipboard (v2)

**Responsabilidade:** integração com `wl_data_device`.

**Funções:**
```c
void clipboard_wl_init(term_t *t);
void clipboard_wl_offer(term_t *t, const char *mime, int fd);
void clipboard_wl_receive(term_t *t, const char *mime, int fd);
```

### 20. `config.c` — Configuração

**Estrutura:**
```c
typedef struct {
    char    font_family[128];
    int     font_size;
    float   dpi;
    char    font_bold[128];
    char    font_italic[128];

    uint32_t bg, fg, cursor;
    uint32_t palette[256];

    cursor_style_t cursor_style;
    bool    cursor_blink;
    int     cursor_blink_interval;

    int     scrollback_lines;

    bool    alt_sends_escape;
    bool    shift_pgup_scroll;
    bool    mouse_tracking;

    // Keybindings
    struct { uint32_t mods; uint32_t sym; char action[32]; } bindings[64];
    int     nbindings;
} config_t;
```

**Funções:**
```c
void config_load(config_t *c, const char *path);
void config_reload(config_t *c);
void config_defaults(config_t *c);
static void config_parse_line(config_t *c, const char *line);
```

---

## PARTE IV — ESTRUTURA DE ARQUIVOS

```
ovsb-term/
├── Makefile
├── README.md
├── LICENSE
├── ARCHITECTURE.md             # este arquivo (blueprint completo)
├── src/
│   ├── main.c                  # Entry point
│   ├── term.h                  # Struct principal (term_t)
│   ├── config.c/.h             # Configuração
│   ├── log.c/.h                # Logging
│   ├── util.c/.h               # Utilidades
│   │
│   ├── wl_client.c/.h          # Wayland client
│   ├── loop.c/.h               # Event loop
│   │
│   ├── grid.c/.h               # Modelo de células
│   ├── scrollback.c/.h         # Histórico
│   ├── selection.c/.h          # Seleção
│   ├── clipboard.c/.h          # Clipboard
│   │
│   ├── escape.c/.h             # Parser de escapes
│   ├── input.c/.h              # Input (kbd/mouse)
│   ├── pty.c/.h                # PTY
│   │
│   ├── font.c/.h               # Font manager
│   ├── render.c/.h             # Render engine
│   ├── glyph_cache.c/.h        # Cache de glifos
│   ├── cursor.c/.h             # Cursor
│   │
│   ├── shm.c/.h                # Buffer Wayland
│   ├── timer.c/.h              # Timerfd
│   ├── signal.c/.h             # Sinais
│   │
│   └── v2/                     # Versão 2 (features extras)
│       ├── sixel.c/.h          # Sixel graphics
│       ├── kitty_gfx.c/.h      # Kitty graphics protocol
│       ├── hyperlink.c/.h      # OSC 8
│       ├── url_detect.c/.h     # Detecção de URL
│       ├── tabs.c/.h           # Abas
│       ├── split.c/.h          # Split panes
│       └── scrollbar.c/.h      # Barra de scroll
├── config/
│   └── ovsb.term.conf          # Config padrão
├── terminfo/
│   └── ovsb.term               # Entrada terminfo
├── tests/
│   ├── test_grid.c
│   ├── test_escape.c
│   ├── test_pty.c
│   ├── test_render.c
│   └── test_input.c
└── docs/
    ├── ARCHITECTURE.md
    ├── PROTOCOL.md
    └── KEYBINDINGS.md
```

---

## PARTE V — TERMINFO COMPLETO

O `ovsb.term` precisa de uma entrada `terminfo` própria. Sem isso, `vim`/`htop`/`tmux` reclamam.

**Arquivo `terminfo/ovsb.term` (fonte):**
```
ovsb.term|OvsbMk Terminal,
    am, bce, ccc, hs, km, mir, msgr, xenl,
    colors#256, cols#80, it#8, lines#24, pairs#32767,
    acsc=``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~,
    bel=^G, blink=\E[5m, bold=\E[1m, cbt=\E[Z,
    civis=\E[?25l, clear=\E[H\E[2J,
    cnorm=\E[?12l\E[?25h, cr=\r,
    csr=\E[%i%p1%d;%p2%dr,
    cub=\E[%p1%dD, cub1=^H,
    cud=\E[%p1%dB, cud1=\n,
    cuf=\E[%p1%dC, cuf1=\E[C,
    cup=\E[%i%p1%d;%p2%dH, cuu=\E[%p1%dA, cuu1=\E[A,
    cvvis=\E[?12;25h,
    dch=\E[%p1%dP, dch1=\E[P, dim=\E[2m,
    dl=\E[%p1%dM, dl1=\E[M, ech=\E[%p1%dX,
    ed=\E[J, el=\E[K, el1=\E[1K,
    flash=\E[?5h$<100/>\E[?5l,
    home=\E[H, hpa=\E[%i%p1%dG,
    ich=\E[%p1%d@, il=\E[%p1%dL, il1=\E[L,
    ind=\n, indn=\E[%p1%dS,
    invis=\E[8m, is2=\E[!p\E[?3;4l\E[4l\E>,
    kDC=\E[3~, kEND=\E[4~, kHOM=\E[1~,
    kIC=\E[2~, kLFT=\E[D, kNXT=\E[6~,
    kPRV=\E[5~, kRIT=\E[C, ka1=\EOq, ka3=\EOs,
    kb2=\EOr, kbs=^?, kc1=\EOp, kc3=\EOn,
    kdch1=\E[3~, kend=\E[4~, kf1=\EOP, kf10=\E[21~,
    kf11=\E[23~, kf12=\E[24~, kf2=\EOQ, kf3=\EOR,
    kf4=\EOS, kf5=\E[15~, kf6=\E[17~, kf7=\E[18~,
    kf8=\E[19~, kf9=\E[20~, khome=\E[1~,
    kich1=\E[2~, knp=\E[6~, kpp=\E[5~,
    op=\E[39;49m, rc=\E8, rev=\E[7m, ri=\EM,
    rmir=\E[4l, rmso=\E[27m, rmul=\E[24m,
    rs2=\E[!p\E[?3;4l\E[4l\E>,
    sc=\E7, setab=\E[%p1%{48}%+%dm,
    setaf=\E[%p1%{48}%+%dm,
    setb=\E[4%p1%dm, setf=\E[3%p1%dm,
    sgr0=\E(B\E[m, sitm=\E[3m, smir=\E[4h,
    smso=\E[7m, smul=\E[4m, tbc=\E[3g,
    u6=\E[%i%d;%dR, u7=\E[6n, u8=\E[?%[;0123456789]c,
    u9=\E[c, vpa=\E[%i%p1%dd,
    Ms=\E]52;%p1%s;%p2%s\007,
    Se=\E[2 q, Ss=\E[%p1%d q,
```

**Onde instalar:** `::/USR/SHARE/TERMINFO/O/OVSB.TERM`

**Como gerar:** `tic -x -o /path terminfo/ovsb.term` no host, e copiar o binário gerado pro `disk.img`.

**Fallback:** se `terminfo` não estiver disponível, o `ovsb.term` usa `TERM=xterm-256color`.

---

## PARTE VI — CONFIGURAÇÃO COMPLETA

**Arquivo `config/ovsb.term.conf`:**

```ini
[main]
term = ovsb.term
shell = /bin/sh
login_shell = false

[font]
family = monospace
size = 12
dpi = 96
bold_family = monospace:style=Bold
italic_family = monospace:style=Italic
bold_italic_family = monospace:style=Bold:style=Italic
antialias = true
hinting = true

[colors]
background = #1e1e1e
foreground = #d4d4d4
cursor = #ffffff
selection_bg = #264f78
selection_fg = #ffffff
palette_0 = #000000
palette_1 = #cd3131
palette_2 = #0dbc79
palette_3 = #e5e510
palette_4 = #2472c8
palette_5 = #bc3fbc
palette_6 = #11a8cd
palette_7 = #e5e5e5
palette_8 = #666666
palette_9 = #f14c4c
palette_10 = #23d18b
palette_11 = #f5f543
palette_12 = #3b8eea
palette_13 = #d670d6
palette_14 = #29b8db
palette_15 = #e5e5e5

[cursor]
style = block           ; block, underline, bar, block_hollow
blink = true
blink_interval = 500    ; ms
color = #ffffff

[scrollback]
lines = 10000

[input]
alt_sends_escape = true
shift_pgup_scroll = true
mouse_tracking = true

[keybindings]
copy = Ctrl+Shift+C
paste = Ctrl+Shift+V
select_all = Ctrl+Shift+A
new_tab = Ctrl+Shift+T
close_tab = Ctrl+Shift+W
next_tab = Ctrl+Page_Down
prev_tab = Ctrl+Page_Up
scroll_up = Shift+Page_Up
scroll_down = Shift+Page_Down
scroll_top = Shift+Home
scroll_bottom = Shift+End
font_increase = Ctrl+plus
font_decrease = Ctrl+minus
font_reset = Ctrl+0
fullscreen = F11
```

---

## PARTE VII — ROADMAP COMPLETO

### v1.0 — Núcleo Funcional

| # | Módulo | Tempo |
|---|---|---|
| 1 | `main.c` + `term.h` | 1 dia |
| 2 | `wl_client.c` (Wayland) | 2 dias |
| 3 | `shm.c` (buffers) | 1 dia |
| 4 | `font.c` (fcft) | 2 dias |
| 5 | `render.c` (pixman) | 3 dias |
| 6 | `pty.c` (shell) | 1 dia |
| 7 | `grid.c` (células) | 2 dias |
| 8 | `escape.c` (parser) | 4 dias |
| 9 | `loop.c` (event loop) | 2 dias |
| 10 | `input.c` (XKB) | 3 dias |
| 11 | `scrollback.c` | 1 dia |
| 12 | `cursor.c` | 1 dia |
| 13 | `glyph_cache.c` | 1 dia |
| 14 | `terminfo` | 1 dia |
| 15 | `config.c` | 1 dia |
| 16 | Testes + debug | 1 semana |
| **Total** | | **~5 semanas** |

### v2.0 — Features Avançadas

| # | Módulo | Tempo |
|---|---|---|
| 1 | `selection.c` (cópia/cola) | 2 dias |
| 2 | `clipboard.c` (wl_data_device) | 2 dias |
| 3 | Ligatures (harfbuzz) | 2 dias |
| 4 | Grapheme clusters | 2 dias |
| 5 | Bidi (RTL) | 1 semana |
| 6 | Mouse tracking completo | 3 dias |
| 7 | Bracketed paste | 1 dia |
| 8 | HiDPI | 1 dia |
| 9 | Abas | 3 dias |
| 10 | Split | 3 dias |
| 11 | Scrollbar | 2 dias |
| 12 | URL detection | 2 dias |
| 13 | Hyperlinks (OSC 8) | 2 dias |
| **Total** | | **~4 semanas** |

### v3.0 — Features Raras

| # | Módulo | Tempo |
|---|---|---|
| 1 | Sixel graphics | 1 semana |
| 2 | Kitty graphics protocol | 1 semana |
| 3 | iTerm2 inline images | 1 semana |
| 4 | GPU rendering (se OvsbMk ganhar GPU) | 2 semanas |
| **Total** | | **~5 semanas** |

---

## PARTE VIII — DEPENDÊNCIAS

### Libs já portadas (reusar)

| Lib | Caminho | Uso |
|---|---|---|
| `libwayland-client.a` | `wl-build/wayland/` | Conexão Wayland |
| `libwayland-cursor.a` | `wl-build/wayland/` | Cursor (opcional) |
| `libwayland-util.a` | `wl-build/wayland/` | Utilidades |
| `libffi.a` | `wl-build/libffi/` | Usada pela `libwayland-client` |
| `libpixman-1.a` | `wl-build/pixman/` | Composição |
| `libharfbuzz.a` | `wl-build/harfbuzz/` | Shaping |
| `libfcft.a` | `wl-build/fcft/` | Rasterização |
| `libxkbcommon.a` | `wl-build/xkbcommon/` | Teclado |
| `libfreetype.a` | host | Glifos |
| `libfontconfig.a` | host | Fontes |
| `libutf8proc.a` | host | UTF-8 |

### O que **não** precisa

- SDL2
- X11
- EGL / OpenGL
- GLib / GTK / Qt
- systemd / dbus
- ncurses / terminfo (externo)

---

## PARTE IX — MAKEFILE COMPLETO

```makefile
# ovsb.term — Makefile
CC       := gcc
STRIP    := strip
CFLAGS   := -static -O2 -g0 -Wall -Wextra -Wno-unused-parameter \
            -std=c11 -D_GNU_SOURCE -fno-stack-protector \
            -mno-sse -mno-mmx -mgeneral-regs-only \
            -Iwl-build/wayland/include \
            -Iwl-build/fcft/include \
            -Iwl-build/pixman/include \
            -Iwl-build/xkbcommon/include \
            -Iwl-build/harfbuzz/include \
            -I/usr/include/freetype2 \
            -I/usr/include/libpng16 \
            -I/usr/include/fontconfig
LDFLAGS  := -static -lpthread -lm -lrt

LIBS := wl-build/fcft/libfcft.a \
        wl-build/harfbuzz/libharfbuzz.a \
        wl-build/pixman/libpixman-1.a \
        wl-build/xkbcommon/libxkbcommon.a \
        wl-build/wayland/libwayland-client.a \
        wl-build/wayland/libwayland-cursor.a \
        wl-build/wayland/libwayland-util.a \
        wl-build/libffi/libffi.a \
        /usr/lib/x86_64-linux-gnu/libfontconfig.a \
        /usr/lib/x86_64-linux-gnu/libfreetype.a \
        /usr/lib/x86_64-linux-gnu/libutf8proc.a \
        /usr/lib/x86_64-linux-gnu/libpng.a \
        -lz -lbz2 -lbrotlidec -lbrotlicommon -lexpat

SRCS := $(wildcard src/*.c) $(wildcard src/v2/*.c)
OBJS := $(SRCS:.c=.o)

TARGET := ovsb.term

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(STRIP) --strip-all $@
	@ls -lh $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	mcopy -o -i ../disk.img $(TARGET) ::/BIN/OVSB.TERM
	mkdir -p ../disk.img.d/usr/share/terminfo/o
	cp terminfo/ovsb.term ../disk.img.d/usr/share/terminfo/o/ovsb.term

.PHONY: all clean install
```

---

## PARTE X — FLUXO COMPLETO DE EXECUÇÃO

### Inicialização

```
1. main()
   ├─ config_load("::/ETC/OVSB.TERM.CONF")
   ├─ wl_init()
   │   ├─ wl_display_connect(NULL)         → /tmp/wayland-0
   │   ├─ wl_display_get_registry()
   │   ├─ wl_registry_add_listener()
   │   ├─ wl_display_roundtrip()           → recebe globals
   │   ├─ wl_compositor_create_surface()
   │   ├─ xdg_wm_base_get_xdg_surface()
   │   ├─ xdg_surface_get_toplevel()
   │   ├─ xdg_toplevel_set_title("ovsb.term")
   │   ├─ wl_surface_commit()
   │   └─ wl_display_roundtrip()           → recebe configure
   ├─ font_init()
   │   ├─ fcft_init()
   │   └─ fcft_from_name("monospace:size=12")
   ├─ grid_init(80, 24)
   ├─ scrollback_init(10000)
   ├─ render_init()
   │   ├─ memfd_create() + mmap()
   │   ├─ wl_shm_create_pool()
   │   └─ wl_shm_pool_create_buffer() × 2
   ├─ pty_init()
   │   ├─ posix_openpt()
   │   ├─ grantpt() + unlockpt()
   │   ├─ fork()
   │   └─ filho: setsid() + execve("/bin/sh")
   ├─ input_init()
   │   ├─ xkb_context_new()
   │   ├─ xkb_keymap_new_from_names()
   │   └─ xkb_state_new()
   └─ loop_init()
       └─ epoll_create() + add fds
```

### Loop Principal

```
2. loop_run()
   └─ while (running)
       ├─ wl_display_dispatch_pending()
       ├─ wl_display_flush()
       ├─ epoll_wait()
       │
       ├─ [wayland_fd] wl_display_dispatch()
       │   ├─ xdg_surface_configure() → render_resize()
       │   ├─ keyboard_key() → input_key() → pty_write()
       │   └─ pointer_*() → input_pointer_*()
       │
       ├─ [pty_fd] read()
       │   ├─ escape_feed()
       │   │   └─ grid_put_char() / grid_cursor_*() / grid_erase_*()
       │   └─ render_update()
       │       ├─ for each dirty row:
       │       │   ├─ render_bg()
       │       │   ├─ font_glyph() (com cache)
       │       │   └─ render_glyph()
       │       ├─ render_cursor()
       │       ├─ wl_surface_attach()
       │       └─ wl_surface_commit()
       │
       ├─ [signal_fd] read()
       │   ├─ SIGCHLD → pty_shell_exited()
       │   ├─ SIGWINCH → pty_resize()
       │   └─ SIGTERM → running = false
       │
       └─ [timer_fd] read()
           ├─ cursor blink → render_cursor()
           └─ refresh
```

### Encerramento

```
3. term_cleanup()
   ├─ pty_cleanup()      → kill(child), close(master)
   ├─ render_cleanup()   → munmap, wl_buffer_destroy
   ├─ grid_cleanup()     → free(cells)
   ├─ font_cleanup()     → fcft_destroy()
   ├─ input_cleanup()    → xkb_state_unref()
   └─ wl_cleanup()       → wl_display_disconnect()
```

---

## PARTE XI — ESTIMATIVA TOTAL

| Item | Linhas de código | Tempo |
|---|---|---|
| **Núcleo (v1)** | ~5.000 | 5 semanas |
| **Features avançadas (v2)** | ~3.000 | 4 semanas |
| **Features raras (v3)** | ~2.000 | 5 semanas |
| **Total** | **~10.000 linhas** | **~14 semanas (~3,5 meses)** |

**Para comparação:** o `foot` tem ~15.000 linhas. O `ovsb.term` teria **~10.000** — mas com **arquitetura própria**, **adaptada ao OvsbMk**, e **sem dependências que o OvsbMk não tem**.

---

**Fim da arquitetura.** Blueprint completo — pronto pra implementar, em qualquer ordem, por qualquer agente.
