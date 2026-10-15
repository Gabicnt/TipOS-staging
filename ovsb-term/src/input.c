/* ovsb.term — input.c — teclado XKB + mouse (moe moe kyun <3)
 *
 * Traduz eventos Wayland em bytes pro pty. Teclas especiais viram
 * sequências xterm; texto vai direto. Mouse vira SGR/X10 ou seleção.
 */
#include "term.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/* botões evdev (sem <linux/input.h> p/ compilar em todo lugar) */
#define BTN_LEFT   0x110u
#define BTN_MIDDLE 0x111u
#define BTN_RIGHT  0x112u

/* modos de mouse (input.mouse_mode) */
#define MOUSE_X10 1u
#define MOUSE_SGR 2u

void input_init(term_t *t)
{
    t->input.xkb_ctx = NULL;
    t->input.xkb_keymap = NULL;
    t->input.xkb_state = NULL;
    t->input.repeat_rate = 30;
    t->input.repeat_delay = 500;
    t->input.mods_depressed = 0;
    t->input.mods_latched = 0;
    t->input.mods_locked = 0;
    t->input.mods_group = 0;
    t->input.mouse_tracking = false;
    t->input.mouse_mode = 0;
    t->input.mouse_x = 0;
    t->input.mouse_y = 0;
    t->input.mouse_buttons = 0;
    t->input.selecting = false;
    t->input.sel_start_x = 0;
    t->input.sel_start_y = 0;

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx)
        return; /* segue sem xkb, com tabela interna */
    struct xkb_keymap *map =
        xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) {
        xkb_context_unref(ctx);
        return;
    }
    struct xkb_state *st = xkb_state_new(map);
    if (!st) {
        xkb_keymap_unref(map);
        xkb_context_unref(ctx);
        return;
    }
    t->input.xkb_ctx = ctx;
    t->input.xkb_keymap = map;
    t->input.xkb_state = st;
}

void input_cleanup(term_t *t)
{
    if (t->input.xkb_state) {
        xkb_state_unref(t->input.xkb_state);
        t->input.xkb_state = NULL;
    }
    if (t->input.xkb_keymap) {
        xkb_keymap_unref(t->input.xkb_keymap);
        t->input.xkb_keymap = NULL;
    }
    if (t->input.xkb_ctx) {
        xkb_context_unref(t->input.xkb_ctx);
        t->input.xkb_ctx = NULL;
    }
}

void input_modifiers(term_t *t, uint32_t dep, uint32_t latched,
                     uint32_t locked, uint32_t group)
{
    t->input.mods_depressed = dep;
    t->input.mods_latched = latched;
    t->input.mods_locked = locked;
    t->input.mods_group = group;
    if (t->input.xkb_state)
        xkb_state_update_mask(t->input.xkb_state, dep, latched, locked,
                              0, 0, group);
}

/* mods ativos via xkb (0 sem xkb) */
static void input_mods(term_t *t, bool *ctrl, bool *alt, bool *shift)
{
    *ctrl = *alt = *shift = false;
    if (!t->input.xkb_state)
        return;
    *ctrl = xkb_state_mod_name_is_active(t->input.xkb_state,
                XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) != 0;
    *alt = xkb_state_mod_name_is_active(t->input.xkb_state,
                XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) != 0;
    *shift = xkb_state_mod_name_is_active(t->input.xkb_state,
                XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) != 0;
}

static void send_str(term_t *t, const char *s)
{
    pty_write(t, s, strlen(s));
}

/* tecla CSI com modificadores: montador genérico */
static void send_mod_csi(term_t *t, char kind, int p1, char p1c,
                         char final)
{
    bool ctrl, alt, shift;
    input_mods(t, &ctrl, &alt, &shift);
    int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    char buf[32];
    int n;
    if (m == 1) {
        if (kind == '~')
            n = snprintf(buf, sizeof(buf), "\x1b[%d~", p1);
        else if (kind == 's')
            n = snprintf(buf, sizeof(buf), "\x1bO%c", p1c);
        else
            n = snprintf(buf, sizeof(buf), "\x1b[%c", final);
    } else {
        if (kind == '~')
            n = snprintf(buf, sizeof(buf), "\x1b[%d;%d~", p1, m);
        else if (kind == 's')
            n = snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, p1c);
        else
            n = snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, final);
    }
    if (n > 0)
        pty_write(t, buf, (size_t)n);
}

/* fallback sem xkb: evdev -> sequência (só especiais) */
static bool fallback_special(term_t *t, uint32_t keycode)
{
    bool app = t->grid.app_cursor;
    switch (keycode) {
    case 1: send_str(t, "\x1b"); return true;
    case 14: pty_write(t, "\x7f", 1); return true;
    case 15: pty_write(t, "\t", 1); return true;
    case 28: pty_write(t, "\r", 1); return true;
    case 102: send_str(t, app ? "\x1bOH" : "\x1b[H"); return true;
    case 103: send_str(t, app ? "\x1bOA" : "\x1b[A"); return true;
    case 104: send_str(t, "\x1b[5~"); return true;
    case 105: send_str(t, app ? "\x1bOD" : "\x1b[D"); return true;
    case 106: send_str(t, app ? "\x1bOC" : "\x1b[C"); return true;
    case 107: send_str(t, app ? "\x1bOF" : "\x1b[F"); return true;
    case 108: send_str(t, app ? "\x1bOB" : "\x1b[B"); return true;
    case 109: send_str(t, "\x1b[6~"); return true;
    case 110: send_str(t, "\x1b[2~"); return true;
    case 111: send_str(t, "\x1b[3~"); return true;
    case 59: send_str(t, "\x1bOP"); return true;
    case 60: send_str(t, "\x1bOQ"); return true;
    case 61: send_str(t, "\x1bOR"); return true;
    case 62: send_str(t, "\x1bOS"); return true;
    case 63: send_str(t, "\x1b[15~"); return true;
    case 64: send_str(t, "\x1b[17~"); return true;
    case 65: send_str(t, "\x1b[18~"); return true;
    case 66: send_str(t, "\x1b[19~"); return true;
    case 67: send_str(t, "\x1b[20~"); return true;
    case 68: send_str(t, "\x1b[21~"); return true;
    case 87: send_str(t, "\x1b[23~"); return true;
    case 88: send_str(t, "\x1b[24~"); return true;
    default: return false;
    }
}

void input_key(term_t *t, uint32_t keycode, uint32_t state)
{
    if (state == 0)
        return; /* press apenas */

    /* sem xkb: tabela interna */
    if (!t->input.xkb_state) {
        fallback_special(t, keycode);
        return;
    }

    xkb_keycode_t kc = (xkb_keycode_t)(keycode + 8);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(t->input.xkb_state, kc);
    char utf8[64];
    int n = xkb_state_key_get_utf8(t->input.xkb_state, kc, utf8,
                                   sizeof(utf8));
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(utf8) - 1)
        n = (int)sizeof(utf8) - 1;

    bool ctrl, alt, shift;
    input_mods(t, &ctrl, &alt, &shift);
    bool app = t->grid.app_cursor;

    /* --- teclas especiais --- */
    switch (sym) {
    case XKB_KEY_Up:
        if (shift || alt || ctrl) send_mod_csi(t, 'c', 0, 0, 'A');
        else send_str(t, app ? "\x1bOA" : "\x1b[A");
        return;
    case XKB_KEY_Down:
        if (shift || alt || ctrl) send_mod_csi(t, 'c', 0, 0, 'B');
        else send_str(t, app ? "\x1bOB" : "\x1b[B");
        return;
    case XKB_KEY_Right:
        if (shift || alt || ctrl) send_mod_csi(t, 'c', 0, 0, 'C');
        else send_str(t, app ? "\x1bOC" : "\x1b[C");
        return;
    case XKB_KEY_Left:
        if (shift || alt || ctrl) send_mod_csi(t, 'c', 0, 0, 'D');
        else send_str(t, app ? "\x1bOD" : "\x1b[D");
        return;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        if (shift || alt || ctrl) send_mod_csi(t, 'h', 1, 0, 'H');
        else send_str(t, app ? "\x1bOH" : "\x1b[H");
        return;
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
        if (shift || alt || ctrl) send_mod_csi(t, 'h', 1, 0, 'F');
        else send_str(t, app ? "\x1bOF" : "\x1b[F");
        return;
    case XKB_KEY_Prior:
    case XKB_KEY_KP_Prior:
        if (shift && !ctrl && !alt && t->config.shift_pgup_scroll) {
            int page = t->grid.rows > 0 ? t->grid.rows : 24;
            scrollback_scroll(t, page);
            return;
        }
        send_mod_csi(t, '~', 5, 0, 0);
        return;
    case XKB_KEY_Next:
    case XKB_KEY_KP_Next:
        if (shift && !ctrl && !alt && t->config.shift_pgup_scroll) {
            int page = t->grid.rows > 0 ? t->grid.rows : 24;
            scrollback_scroll(t, -page);
            return;
        }
        send_mod_csi(t, '~', 6, 0, 0);
        return;
    case XKB_KEY_Insert:
    case XKB_KEY_KP_Insert:
        send_mod_csi(t, '~', 2, 0, 0);
        return;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        send_mod_csi(t, '~', 3, 0, 0);
        return;
    case XKB_KEY_F1: send_mod_csi(t, 's', 0, 'P', 0); return;
    case XKB_KEY_F2: send_mod_csi(t, 's', 0, 'Q', 0); return;
    case XKB_KEY_F3: send_mod_csi(t, 's', 0, 'R', 0); return;
    case XKB_KEY_F4: send_mod_csi(t, 's', 0, 'S', 0); return;
    case XKB_KEY_F5: send_mod_csi(t, '~', 15, 0, 0); return;
    case XKB_KEY_F6: send_mod_csi(t, '~', 17, 0, 0); return;
    case XKB_KEY_F7: send_mod_csi(t, '~', 18, 0, 0); return;
    case XKB_KEY_F8: send_mod_csi(t, '~', 19, 0, 0); return;
    case XKB_KEY_F9: send_mod_csi(t, '~', 20, 0, 0); return;
    case XKB_KEY_F10: send_mod_csi(t, '~', 21, 0, 0); return;
    case XKB_KEY_F11: send_mod_csi(t, '~', 23, 0, 0); return;
    case XKB_KEY_F12: send_mod_csi(t, '~', 24, 0, 0); return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (alt && !ctrl)
            send_str(t, "\x1b\r");
        else
            pty_write(t, "\r", 1);
        return;
    case XKB_KEY_Tab:
        if (shift && !ctrl && !alt)
            send_str(t, "\x1b[Z"); /* CBT */
        else if (alt && !ctrl)
            send_str(t, "\x1b\t");
        else
            pty_write(t, "\t", 1);
        return;
    case XKB_KEY_ISO_Left_Tab:
        send_str(t, "\x1b[Z");
        return;
    case XKB_KEY_BackSpace:
        if (alt && !ctrl)
            send_str(t, "\x1b\x7f");
        else
            pty_write(t, "\x7f", 1);
        return;
    case XKB_KEY_Escape:
        if (alt && !ctrl)
            send_str(t, "\x1b\x1b");
        else
            pty_write(t, "\x1b", 1);
        return;
    default:
        break;
    }

    /* --- Ctrl+A-Z -> 0x01-0x1A --- */
    if (ctrl && !alt) {
        xkb_keysym_t lo = sym;
        if (lo >= XKB_KEY_A && lo <= XKB_KEY_Z)
            lo = lo - XKB_KEY_A + XKB_KEY_a;
        if (lo >= XKB_KEY_a && lo <= XKB_KEY_z) {
            char c = (char)(lo - XKB_KEY_a + 1);
            pty_write(t, &c, 1);
            return;
        }
        if (sym == XKB_KEY_space) {
            char c = 0x00;
            pty_write(t, &c, 1);
            return;
        }
        if (n > 0) {
            /* xkb já aplicou o ctrl (ex veio controle): passa direto */
            pty_write(t, utf8, (size_t)n);
            return;
        }
        return;
    }

    /* --- texto normal (com Alt -> ESC+char) --- */
    if (n > 0) {
        if (alt && t->config.alt_sends_escape) {
            pty_write(t, "\x1b", 1);
            pty_write(t, utf8, (size_t)n);
        } else {
            /* bracketed-paste-safe: digitação vai direta, sem wrap */
            pty_write(t, utf8, (size_t)n);
        }
    }
}

/* envia evento de mouse (x,y em células 0-based) */
static void send_mouse(term_t *t, int button, bool press, int x, int y)
{
    bool ctrl, alt, shift;
    input_mods(t, &ctrl, &alt, &shift);
    int b;
    if (!press) {
        b = 3; /* release */
    } else {
        b = button;
        if (shift) b += 4;
        if (alt) b += 8;
        if (ctrl) b += 16;
    }
    int cx = x + 1, cy = y + 1; /* protocolo é 1-based */
    if (cx < 1) cx = 1;
    if (cy < 1) cy = 1;
    char buf[32];
    int n;
    if (t->input.mouse_mode == MOUSE_SGR) {
        n = snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", b, cx, cy,
                     press ? 'M' : 'm');
    } else {
        if (cx > 223) cx = 223;
        if (cy > 223) cy = 223;
        n = snprintf(buf, sizeof(buf), "\x1b[M%c%c%c",
                     (char)(32 + b), (char)(32 + cx), (char)(32 + cy));
    }
    if (n > 0)
        pty_write(t, buf, (size_t)n);
}

static bool tracking_on(term_t *t)
{
    return t->input.mouse_tracking || t->grid.mouse_tracking;
}

static bool shift_held(term_t *t)
{
    bool ctrl, alt, shift;
    input_mods(t, &ctrl, &alt, &shift);
    (void)ctrl;
    (void)alt;
    return shift;
}

void input_pointer_motion(term_t *t, int sx, int sy)
{
    int cw = t->font.cell_width > 0 ? t->font.cell_width : 8;
    int ch = t->font.cell_height > 0 ? t->font.cell_height : 16;
    int x = (int)(sx / cw);
    int y = (int)(sy / ch);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    t->input.mouse_x = x;
    t->input.mouse_y = y;
    if (t->input.selecting)
        selection_extend(t, x, y);
}

void input_pointer_button(term_t *t, uint32_t button, uint32_t state)
{
    bool press = (state == 1);
    int x = t->input.mouse_x;
    int y = t->input.mouse_y;

    int btn = -1;
    if (button == BTN_LEFT) btn = 0;
    else if (button == BTN_MIDDLE) btn = 1;
    else if (button == BTN_RIGHT) btn = 2;

    /* modo app: botão esquerdo sem Shift vira evento pro pty */
    if (tracking_on(t) && btn >= 0 && button == BTN_LEFT &&
        !shift_held(t)) {
        send_mouse(t, btn, press, x, y);
        return;
    }

    if (button == BTN_MIDDLE && press && !tracking_on(t)) {
        clipboard_paste(t); /* colar com botão do meio */
        return;
    }

    if (button != BTN_LEFT)
        return;

    if (press) {
        if (!t->input.selecting) {
            t->input.selecting = true;
            t->input.sel_start_x = x;
            t->input.sel_start_y = y;
            selection_start(t, x, y);
        }
    } else {
        if (t->input.selecting) {
            t->input.selecting = false;
            selection_finish(t);
            char *txt = selection_get_text(t);
            if (txt) {
                if (txt[0] != '\0')
                    clipboard_set(t, txt, strlen(txt));
                free(txt);
            }
        }
    }
}

void input_pointer_axis(term_t *t, uint32_t axis, int value)
{
    if (tracking_on(t)) {
        /* roda vira botão 64/65 */
        int dir = (value < 0) ? 0 : 1;
        if (axis == 0)
            send_mouse(t, 64 + dir, true, t->input.mouse_x,
                       t->input.mouse_y);
        return;
    }
    if (axis == 0) {
        /* cima (negativo) = linhas antigas */
        scrollback_scroll(t, value < 0 ? 3 : -3);
    }
}
