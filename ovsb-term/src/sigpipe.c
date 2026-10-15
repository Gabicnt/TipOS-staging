/* ovsb.term — sigpipe.c — self-pipe async-safe (moe moe kyun <3)
 * (chamava-se signal.c; renomeado p/ não sombrear <signal.h> com -Isrc) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "term.h"
#include "sigpipe.h"

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* fd de escrita visível ao handler (só write aqui!) */
static int g_sig_wfd = -1;
/* ações antigas p/ restaurar no cleanup */
static struct sigaction g_old_chld;
static struct sigaction g_old_winch;
static struct sigaction g_old_term;
static bool g_sig_installed = false;

static void sig_handler(int sig)
{
    if (g_sig_wfd < 0)
        return;
    int saved = errno;
    unsigned char b = (unsigned char)sig;
    /* async-safe: só write; ignora EAGAIN (pipe cheio) */
    (void)write(g_sig_wfd, &b, 1);
    errno = saved;
}

void signal_init(term_t *t)
{
    if (!t)
        return;
    t->loop.signal_pipe[0] = -1;
    t->loop.signal_pipe[1] = -1;

    int p[2];
    if (pipe(p) < 0)
        return;
    /* nonblock nas duas pontas */
    int fl0 = fcntl(p[0], F_GETFL, 0);
    int fl1 = fcntl(p[1], F_GETFL, 0);
    if (fl0 >= 0)
        (void)fcntl(p[0], F_SETFL, fl0 | O_NONBLOCK);
    if (fl1 >= 0)
        (void)fcntl(p[1], F_SETFL, fl1 | O_NONBLOCK);
    /* close-on-exec */
    int fd0 = fcntl(p[0], F_GETFD, 0);
    int fd1 = fcntl(p[1], F_GETFD, 0);
    if (fd0 >= 0)
        (void)fcntl(p[0], F_SETFD, fd0 | FD_CLOEXEC);
    if (fd1 >= 0)
        (void)fcntl(p[1], F_SETFD, fd1 | FD_CLOEXEC);

    t->loop.signal_pipe[0] = p[0];
    t->loop.signal_pipe[1] = p[1];
    g_sig_wfd = p[1];

    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, &g_old_chld) == 0 &&
        sigaction(SIGWINCH, &sa, &g_old_winch) == 0 &&
        sigaction(SIGTERM, &sa, &g_old_term) == 0) {
        g_sig_installed = true;
    }
}

void signal_cleanup(term_t *t)
{
    if (g_sig_installed) {
        (void)sigaction(SIGCHLD, &g_old_chld, NULL);
        (void)sigaction(SIGWINCH, &g_old_winch, NULL);
        (void)sigaction(SIGTERM, &g_old_term, NULL);
        g_sig_installed = false;
    }
    g_sig_wfd = -1;
    if (!t)
        return;
    if (t->loop.signal_pipe[0] >= 0) {
        close(t->loop.signal_pipe[0]);
        t->loop.signal_pipe[0] = -1;
    }
    if (t->loop.signal_pipe[1] >= 0) {
        close(t->loop.signal_pipe[1]);
        t->loop.signal_pipe[1] = -1;
    }
}
