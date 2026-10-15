/* ovsb.term — pty.c — PTY manager (moe moe kyun <3)
 *
 * Abre /dev/ptmx, fork+setsid, configura termios do slave,
 * executa o shell; o pai fala pelo master em nonblock.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "term.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef __has_include
#if __has_include(<utmp.h>)
#include <utmp.h>
#endif
#else
#include <utmp.h>
#endif
/* sem utmp: fallback é não usar utmp (login_tty não é necessário) */

/* configura termios do slave (ARCH §12) */
static void pty_set_termios(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
        return;
    tio.c_iflag = ICRNL | IXON;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CREAD | CS8 | HUPCL;
    tio.c_lflag = ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE |
                  ISIG | IEXTEN;
    tio.c_cc[VINTR]    = 0x03;
    tio.c_cc[VQUIT]    = 0x1C;
    tio.c_cc[VERASE]   = 0x7F;
    tio.c_cc[VKILL]    = 0x15;
    tio.c_cc[VEOF]     = 0x04;
    tio.c_cc[VTIME]    = 0;
    tio.c_cc[VMIN]     = 1;
    tio.c_cc[VSTART]   = 0x11;
    tio.c_cc[VSTOP]    = 0x13;
    tio.c_cc[VSUSP]    = 0x1A;
    tio.c_cc[VREPRINT] = 0x12;
    tio.c_cc[VDISCARD] = 0x0F;
    tio.c_cc[VWERASE]  = 0x17;
    tio.c_cc[VLNEXT]   = 0x16;
    tcsetattr(fd, TCSANOW, &tio);
}

static void pty_set_winsize(int fd, int cols, int rows)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);
    ioctl(fd, TIOCSWINSZ, &ws);
}

bool pty_init(term_t *t)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        /* fallback: abre /dev/ptmx direto */
        master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
        if (master < 0)
            return false;
    }
    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        close(master);
        return false;
    }
    char *path = ptsname(master);
    if (!path) {
        close(master);
        return false;
    }
    snprintf(t->pty.slave_path, sizeof(t->pty.slave_path), "%s", path);

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return false;
    }
    if (pid == 0) {
        /* filho: nova sessão, slave vira stdio, exec shell */
        if (setsid() < 0)
            _exit(127);
        int slave = open(t->pty.slave_path, O_RDWR);
        if (slave < 0)
            _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        pty_set_termios(slave);
        pty_set_winsize(slave, t->grid.cols, t->grid.rows);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        close(master);

        const char *term = t->config.term_name[0] ? t->config.term_name
                                                  : "xterm-256color";
        setenv("TERM", term, 1);
        const char *shell = t->config.shell[0] ? t->config.shell : "/bin/sh";
        if (strchr(shell, '/'))
            execl(shell, shell, (char *)NULL);
        else
            execlp(shell, shell, (char *)NULL);
        /* fallback p/ /bin/sh */
        if (strcmp(shell, "/bin/sh") != 0)
            execl("/bin/sh", "/bin/sh", (char *)NULL);
        _exit(127);
    }

    /* pai: fecha slave (não abriu), master em nonblock */
    int fl = fcntl(master, F_GETFL, 0);
    if (fl >= 0)
        fcntl(master, F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(master, F_GETFD, 0);
    if (fl >= 0)
        fcntl(master, F_SETFD, fl | FD_CLOEXEC);

    t->pty.master_fd = master;
    t->pty.child_pid = pid;
    t->pty.alive = true;
    return true;
}

void pty_cleanup(term_t *t)
{
    if (t->pty.child_pid > 0) {
        kill(t->pty.child_pid, SIGTERM);
        /* espera até ~500ms, depois KILL */
        int status = 0;
        pid_t r = 0;
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000; /* 10ms */
        for (int i = 0; i < 50; i++) {
            r = waitpid(t->pty.child_pid, &status, WNOHANG);
            if (r != 0)
                break;
            nanosleep(&ts, NULL);
        }
        if (r == 0) {
            kill(t->pty.child_pid, SIGKILL);
            waitpid(t->pty.child_pid, &status, 0);
        }
        t->pty.child_pid = 0;
    }
    if (t->pty.master_fd >= 0) {
        close(t->pty.master_fd);
        t->pty.master_fd = -1;
    }
    t->pty.alive = false;
}

ssize_t pty_read(term_t *t, char *buf, size_t len)
{
    if (t->pty.master_fd < 0)
        return -1;
    ssize_t r = read(t->pty.master_fd, buf, len);
    if (r < 0 && errno == EIO)
        return 0; /* slave fechou: EOF */
    return r;
}

ssize_t pty_write(term_t *t, const char *buf, size_t len)
{
    if (t->pty.master_fd < 0 || !buf)
        return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(t->pty.master_fd, buf + done, len - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return done > 0 ? (ssize_t)done : -1;
        }
        if (w == 0)
            break;
        done += (size_t)w;
    }
    if (done == 0 && len > 0)
        return -1; /* nada escrito (EAGAIN), errno preservado */
    return (ssize_t)done;
}

void pty_resize(term_t *t, int cols, int rows)
{
    if (t->pty.master_fd < 0)
        return;
    pty_set_winsize(t->pty.master_fd, cols, rows);
    if (t->pty.alive && t->pty.child_pid > 0)
        kill(t->pty.child_pid, SIGWINCH);
}

void pty_shell_exited(term_t *t)
{
    t->pty.alive = false;
    t->running = false;
}
