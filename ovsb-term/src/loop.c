/* ovsb.term — loop.c — event loop via poll() (moe moe kyun <3) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "term.h"
#include "loop.h"

#include <wayland-client.h>

#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

void loop_init(term_t *t)
{
    if (!t)
        return;
    t->loop.poll_fds_extra = 0;
    t->loop.wl_fd = -1;
    t->loop.pty_fd = -1;
    t->loop.signal_pipe[0] = -1;
    t->loop.signal_pipe[1] = -1;
    t->loop.timer_fd = -1;
    t->loop.blink_on = true;

    if (t->wl.have_display && t->wl.display)
        t->loop.wl_fd = wl_display_get_fd(t->wl.display);
    if (t->pty.master_fd >= 0)
        t->loop.pty_fd = t->pty.master_fd;

    signal_init(t);

    /* timer de blink (só se configurado) */
    int interval = t->config.cursor_blink_interval;
    if (interval <= 0)
        interval = 500;
    if (t->config.cursor_blink)
        t->loop.timer_fd = timer_create_ms(interval, true);
}

void loop_handle_pty(term_t *t)
{
    if (!t)
        return;
    char buf[4096];
    ssize_t r = pty_read(t, buf, sizeof(buf));
    if (r > 0) {
        escape_feed(t, buf, (size_t)r);
        if (t->grid.dirty_count > 0)
            render_update(t);
    } else if (r == 0 || (r < 0 && errno != EAGAIN
#if EAGAIN != EWOULDBLOCK
                          && errno != EWOULDBLOCK
#endif
                          )) {
        pty_shell_exited(t);
    }
}

void loop_handle_timer(term_t *t)
{
    if (!t || t->loop.timer_fd < 0)
        return;
    /* limpa o timerfd (8 bytes) */
    uint64_t exp = 0;
    ssize_t r = read(t->loop.timer_fd, &exp, sizeof(exp));
    (void)r;
    t->loop.blink_on = !t->loop.blink_on;
    render_cursor(t);
}

static void loop_handle_signal(term_t *t)
{
    if (!t || t->loop.signal_pipe[0] < 0)
        return;
    unsigned char buf[32];
    for (;;) {
        ssize_t r = read(t->loop.signal_pipe[0], buf, sizeof(buf));
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == (unsigned char)SIGCHLD) {
                pty_shell_exited(t);
            } else if (buf[i] == (unsigned char)SIGWINCH) {
                pty_resize(t, t->grid.cols, t->grid.rows);
            } else if (buf[i] == (unsigned char)SIGTERM) {
                t->running = false;
            }
        }
        if (r < (ssize_t)sizeof(buf))
            break;
    }
}

void loop_run(term_t *t)
{
    if (!t)
        return;
    /* atualiza fds caso pty/wl tenham sido abertos após loop_init */
    if (t->loop.wl_fd < 0 && t->wl.have_display && t->wl.display)
        t->loop.wl_fd = wl_display_get_fd(t->wl.display);
    if (t->loop.pty_fd < 0 && t->pty.master_fd >= 0)
        t->loop.pty_fd = t->pty.master_fd;

    while (t->running) {
        if (t->wl.have_display && t->wl.display) {
            wl_display_dispatch_pending(t->wl.display);
            if (wl_display_flush(t->wl.display) < 0 && errno != EAGAIN)
                break;
        }

        struct pollfd pf[4];
        int n = 0;
        int idx_wl = -1, idx_pty = -1, idx_sig = -1, idx_tmr = -1;
        if (t->loop.wl_fd >= 0) {
            pf[n].fd = t->loop.wl_fd;
            pf[n].events = POLLIN;
            pf[n].revents = 0;
            idx_wl = n;
            n++;
        }
        if (t->pty.master_fd >= 0) {
            t->loop.pty_fd = t->pty.master_fd;
            pf[n].fd = t->loop.pty_fd;
            pf[n].events = POLLIN;
            pf[n].revents = 0;
            idx_pty = n;
            n++;
        }
        if (t->loop.signal_pipe[0] >= 0) {
            pf[n].fd = t->loop.signal_pipe[0];
            pf[n].events = POLLIN;
            pf[n].revents = 0;
            idx_sig = n;
            n++;
        }
        if (t->loop.timer_fd >= 0) {
            pf[n].fd = t->loop.timer_fd;
            pf[n].events = POLLIN;
            pf[n].revents = 0;
            idx_tmr = n;
            n++;
        }
        if (n == 0)
            break;

        int pr = poll(pf, (nfds_t)n, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!t->running)
            break;

        if (idx_wl >= 0 && (pf[idx_wl].revents & POLLIN)) {
            if (t->wl.display &&
                wl_display_dispatch(t->wl.display) < 0) {
                t->running = false;
                break;
            }
        }
        if (idx_pty >= 0 &&
            (pf[idx_pty].revents & (POLLIN | POLLHUP | POLLERR)))
            loop_handle_pty(t);
        if (idx_sig >= 0 && (pf[idx_sig].revents & POLLIN))
            loop_handle_signal(t);
        if (idx_tmr >= 0 && (pf[idx_tmr].revents & POLLIN))
            loop_handle_timer(t);

        if (!t->pty.alive && t->pty.master_fd < 0) {
            /* shell morreu e pty fechado: encerra */
            break;
        }
    }
}

void loop_cleanup(term_t *t)
{
    if (!t)
        return;
    if (t->loop.timer_fd >= 0) {
        timer_disarm(t->loop.timer_fd);
        close(t->loop.timer_fd);
        t->loop.timer_fd = -1;
    }
    signal_cleanup(t);
    t->loop.wl_fd = -1;
    t->loop.pty_fd = -1;
}
