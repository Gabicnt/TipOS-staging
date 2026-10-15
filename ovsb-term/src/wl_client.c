/* ovsb.term — wl_client.c — Wayland+xdg (padrão wayland_xdg_client.c <3) */
#include "term.h"
#include "wl_client.h"

#include <wayland-client.h>
#include "generated/xdg-shell-client-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

/* xkbcommon opcional: tenta keymap a partir da string, senão ignora */
#if defined(__has_include)
#if __has_include(<xkbcommon/xkbcommon.h>)
#include <xkbcommon/xkbcommon.h>
#define HAVE_XKBCOMMON 1
#endif
#endif

/* fwd */
static void ensure_pointer(term_t *t);
static void ensure_keyboard(term_t *t);

/* ---------- xdg_wm_base ping/pong ---------- */
static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

/* ---------- xdg_surface / toplevel ---------- */
static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    term_t *t = (term_t *)data;
    xdg_surface_ack_configure(xs, serial);
    if (t)
        t->wl.configured = true;
}
static const struct xdg_surface_listener xs_listener = {
    .configure = xs_configure,
};

static void tl_configure(void *data, struct xdg_toplevel *tl,
                         int32_t w, int32_t h, struct wl_array *states)
{
    term_t *t = (term_t *)data;
    (void)tl;
    (void)states;
    /* guarda tamanho sugerido; render_resize é de outro módulo */
    if (t && w > 0 && h > 0) {
        t->wl.width = w;
        t->wl.height = h;
    }
}
static void tl_close(void *data, struct xdg_toplevel *tl)
{
    term_t *t = (term_t *)data;
    (void)tl;
    if (t)
        t->running = false;
}
static const struct xdg_toplevel_listener tl_listener = {
    .configure = tl_configure,
    .close = tl_close,
};

/* ---------- seat ---------- */
static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    term_t *t = (term_t *)data;
    (void)seat;
    if (!t)
        return;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !t->wl.pointer)
        ensure_pointer(t);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !t->wl.keyboard)
        ensure_keyboard(t);
}
static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

/* ---------- pointer → input_* + selection ---------- */
static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy)
{
    term_t *t = (term_t *)data;
    (void)p;
    (void)serial;
    (void)s;
    if (!t)
        return;
    input_pointer_motion(t, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s)
{
    (void)data;
    (void)p;
    (void)serial;
    (void)s;
}
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy)
{
    term_t *t = (term_t *)data;
    (void)p;
    (void)time;
    if (!t)
        return;
    input_pointer_motion(t, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
    /* arrasto com botão = estende seleção (células aproximadas) */
    if (t->input.selecting && t->font.cell_width > 0 &&
        t->font.cell_height > 0) {
        int cx = (int)(wl_fixed_to_int(sx) / t->font.cell_width);
        int cy = (int)(wl_fixed_to_int(sy) / t->font.cell_height);
        selection_extend(t, cx, cy);
    }
}
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state)
{
    term_t *t = (term_t *)data;
    (void)p;
    (void)serial;
    (void)time;
    if (!t)
        return;
    input_pointer_button(t, button, state);
}
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value)
{
    term_t *t = (term_t *)data;
    (void)p;
    (void)time;
    if (!t)
        return;
    input_pointer_axis(t, axis, wl_fixed_to_int(value));
}
static void ptr_frame(void *data, struct wl_pointer *p)
{
    (void)data;
    (void)p;
}
static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t src)
{
    (void)data;
    (void)p;
    (void)src;
}
static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                          uint32_t axis)
{
    (void)data;
    (void)p;
    (void)time;
    (void)axis;
}
static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
                              int32_t disc)
{
    term_t *t = (term_t *)data;
    (void)p;
    if (!t)
        return;
    input_pointer_axis(t, axis, disc);
}
static void ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t axis,
                              int32_t v120)
{
    term_t *t = (term_t *)data;
    (void)p;
    if (!t)
        return;
    input_pointer_axis(t, axis, v120 / 120);
}
static void ptr_axis_rel_dir(void *data, struct wl_pointer *p, uint32_t axis,
                             uint32_t dir)
{
    (void)data;
    (void)p;
    (void)axis;
    (void)dir;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_rel_dir,
};

/* ---------- keyboard → input_* ---------- */
static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t format,
                       int32_t fd, uint32_t size)
{
    term_t *t = (term_t *)data;
    (void)kbd;
    if (!t || fd < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
#ifdef HAVE_XKBCOMMON
    /* tenta montar keymap a partir da string; senão ignora */
    if (size == 0) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    if (!t->input.xkb_ctx)
        t->input.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (t->input.xkb_ctx) {
        struct xkb_keymap *km = xkb_keymap_new_from_string(
            t->input.xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (km) {
            struct xkb_state *st = xkb_state_new(km);
            if (st) {
                if (t->input.xkb_state)
                    xkb_state_unref(t->input.xkb_state);
                if (t->input.xkb_keymap)
                    xkb_keymap_unref(t->input.xkb_keymap);
                t->input.xkb_keymap = km;
                t->input.xkb_state = st;
            } else {
                xkb_keymap_unref(km);
            }
        }
    }
    munmap(map, size);
    close(fd);
#else
    /* sem xkbcommon: ignora keymap */
    close(fd);
#endif
}
static void kbd_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys)
{
    (void)data;
    (void)kbd;
    (void)serial;
    (void)s;
    (void)keys;
}
static void kbd_leave(void *data, struct wl_keyboard *kbd, uint32_t serial,
                      struct wl_surface *s)
{
    (void)data;
    (void)kbd;
    (void)serial;
    (void)s;
}
static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
    term_t *t = (term_t *)data;
    (void)kbd;
    (void)serial;
    (void)time;
    if (!t)
        return;
    input_key(t, key, state);
}
static void kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                          uint32_t dep, uint32_t latched, uint32_t locked,
                          uint32_t group)
{
    term_t *t = (term_t *)data;
    (void)kbd;
    (void)serial;
    if (!t)
        return;
    input_modifiers(t, dep, latched, locked, group);
}
static void kbd_repeat(void *data, struct wl_keyboard *kbd, int32_t rate,
                       int32_t delay)
{
    term_t *t = (term_t *)data;
    (void)kbd;
    if (!t)
        return;
    t->input.repeat_rate = rate;
    t->input.repeat_delay = delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kbd_keymap,
    .enter = kbd_enter,
    .leave = kbd_leave,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat,
};

static void ensure_pointer(term_t *t)
{
    if (!t || !t->wl.seat || t->wl.pointer)
        return;
    struct wl_pointer *p = wl_seat_get_pointer(t->wl.seat);
    if (!p)
        return;
    wl_pointer_add_listener(p, &pointer_listener, t);
    t->wl.pointer = p;
}

static void ensure_keyboard(term_t *t)
{
    if (!t || !t->wl.seat || t->wl.keyboard)
        return;
    struct wl_keyboard *k = wl_seat_get_keyboard(t->wl.seat);
    if (!k)
        return;
    wl_keyboard_add_listener(k, &keyboard_listener, t);
    t->wl.keyboard = k;
}

/* ---------- registry ---------- */
static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver)
{
    term_t *t = (term_t *)data;
    if (!t)
        return;
    if (strcmp(iface, "wl_compositor") == 0) {
        uint32_t v = ver > 4 ? 4 : ver;
        t->wl.compositor =
            wl_registry_bind(reg, name, &wl_compositor_interface, v);
    } else if (strcmp(iface, "wl_shm") == 0) {
        uint32_t v = ver > 1 ? 1 : ver;
        t->wl.shm = wl_registry_bind(reg, name, &wl_shm_interface, v);
    } else if (strcmp(iface, "wl_seat") == 0) {
        uint32_t v = ver > 7 ? 7 : ver;
        if (v < 5)
            v = ver;
        t->wl.seat = wl_registry_bind(reg, name, &wl_seat_interface, v);
        wl_seat_add_listener(t->wl.seat, &seat_listener, t);
        /* como no cliente de referência: pega input já */
        ensure_pointer(t);
        ensure_keyboard(t);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        uint32_t v = ver > 3 ? 3 : ver;
        t->wl.xdg_wm_base =
            wl_registry_bind(reg, name, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(t->wl.xdg_wm_base, &wm_listener, t);
    }
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

/* ---------- API ---------- */
bool wl_init(term_t *t)
{
    if (!t)
        return false;
    memset(&t->wl, 0, sizeof(t->wl));
    t->wl.scale = 1;

    /* tenta: NULL, "wayland-0", $XDG_RUNTIME_DIR/wayland-0, /tmp/wayland-0 */
    struct wl_display *d = wl_display_connect(NULL);
    if (!d)
        d = wl_display_connect("wayland-0");
    if (!d) {
        const char *xd = getenv("XDG_RUNTIME_DIR");
        char path[256];
        if (xd && xd[0])
            snprintf(path, sizeof(path), "%s/wayland-0", xd);
        else
            snprintf(path, sizeof(path), "/tmp/wayland-0");
        d = wl_display_connect(path);
    }
    if (!d) {
        d = wl_display_connect("/tmp/wayland-0");
        if (!d)
            return false;
    }
    t->wl.display = d;
    t->wl.have_display = true;

    t->wl.registry = wl_display_get_registry(d);
    if (!t->wl.registry)
        return false;
    wl_registry_add_listener(t->wl.registry, &reg_listener, t);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);

    if (!t->wl.compositor || !t->wl.shm || !t->wl.xdg_wm_base)
        return false;

    /* garante input mesmo se caps ainda não chegaram */
    ensure_pointer(t);
    ensure_keyboard(t);

    if (t->wl.width <= 0)
        t->wl.width = 640;
    if (t->wl.height <= 0)
        t->wl.height = 480;

    t->wl.surface = wl_compositor_create_surface(t->wl.compositor);
    if (!t->wl.surface)
        return false;
    t->wl.xdg_surface =
        xdg_wm_base_get_xdg_surface(t->wl.xdg_wm_base, t->wl.surface);
    if (!t->wl.xdg_surface)
        return false;
    xdg_surface_add_listener(t->wl.xdg_surface, &xs_listener, t);
    t->wl.xdg_toplevel =
        xdg_surface_get_toplevel(t->wl.xdg_surface);
    if (!t->wl.xdg_toplevel)
        return false;
    xdg_toplevel_add_listener(t->wl.xdg_toplevel, &tl_listener, t);
    xdg_toplevel_set_title(t->wl.xdg_toplevel, "ovsb.term");
    wl_surface_commit(t->wl.surface);
    wl_display_flush(d);

    /* aguarda configure (loop limitado, como a referência) */
    t->wl.configured = false;
    for (int i = 0; i < 100 && !t->wl.configured; i++) {
        if (wl_display_dispatch(d) < 0)
            break;
        if (t->wl.configured)
            break;
        if (i % 20 == 19)
            wl_display_roundtrip(d);
    }
    return true;
}

bool wl_attach(term_t *t)
{
    if (!t || !t->wl.have_display || !t->wl.display || !t->wl.surface)
        return false;
    shm_buffer_t *cur = &t->render.buffers[t->render.current];
    if (!cur->valid || !cur->buffer)
        return false;
    wl_surface_attach(t->wl.surface, cur->buffer, 0, 0);
    wl_surface_damage(t->wl.surface, 0, 0, t->wl.width, t->wl.height);
    wl_surface_commit(t->wl.surface);
    t->render.attached = true;
    return true;
}

void wl_cleanup(term_t *t)
{
    if (!t)
        return;
    /* ordem reversa da criação */
    if (t->wl.xdg_toplevel) {
        xdg_toplevel_destroy(t->wl.xdg_toplevel);
        t->wl.xdg_toplevel = NULL;
    }
    if (t->wl.xdg_surface) {
        xdg_surface_destroy(t->wl.xdg_surface);
        t->wl.xdg_surface = NULL;
    }
    if (t->wl.surface) {
        wl_surface_destroy(t->wl.surface);
        t->wl.surface = NULL;
    }
    if (t->wl.keyboard) {
        wl_keyboard_destroy(t->wl.keyboard);
        t->wl.keyboard = NULL;
    }
    if (t->wl.pointer) {
        wl_pointer_destroy(t->wl.pointer);
        t->wl.pointer = NULL;
    }
    if (t->wl.seat) {
        wl_seat_destroy(t->wl.seat);
        t->wl.seat = NULL;
    }
    if (t->wl.xdg_wm_base) {
        xdg_wm_base_destroy(t->wl.xdg_wm_base);
        t->wl.xdg_wm_base = NULL;
    }
    if (t->wl.shm) {
        wl_shm_destroy(t->wl.shm);
        t->wl.shm = NULL;
    }
    if (t->wl.compositor) {
        wl_compositor_destroy(t->wl.compositor);
        t->wl.compositor = NULL;
    }
    if (t->wl.registry) {
        wl_registry_destroy(t->wl.registry);
        t->wl.registry = NULL;
    }
    if (t->wl.display) {
        wl_display_disconnect(t->wl.display);
        t->wl.display = NULL;
    }
    t->wl.have_display = false;
    t->wl.configured = false;
}
