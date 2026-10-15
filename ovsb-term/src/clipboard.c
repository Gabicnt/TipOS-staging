/* ovsb.term — clipboard.c — memória interna + stubs wl (moe moe kyun <3)
 *
 * v1: clipboard interno (strdup). Sem wl_data_device ainda — offer/receive
 * operam sobre a memória interna via fd (pipe). v2 ligará de verdade no
 * wl_data_device do compositor (ver ARCHITECTURE.md §19).
 */
#include "term.h"
#include "clipboard.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

void clipboard_set(term_t *t, const char *text, size_t len)
{
    if (!t)
        return;
    free(t->clipboard.data);
    t->clipboard.data = NULL;
    t->clipboard.len = 0;
    t->clipboard.is_set = false;
    if (!text || len == 0)
        return;
    char *cp = malloc(len + 1);
    if (!cp)
        return;
    memcpy(cp, text, len);
    cp[len] = '\0';
    t->clipboard.data = cp;
    t->clipboard.len = len;
    t->clipboard.is_set = true;
}

void clipboard_paste(term_t *t)
{
    if (!t || !t->clipboard.is_set || !t->clipboard.data ||
        t->clipboard.len == 0)
        return;
    if (t->grid.bracketed_paste) {
        /* envolve em \e[200~ ... \e[201~ */
        pty_write(t, "\x1b[200~", 6);
        pty_write(t, t->clipboard.data, t->clipboard.len);
        pty_write(t, "\x1b[201~", 6);
    } else {
        pty_write(t, t->clipboard.data, t->clipboard.len);
    }
}

void clipboard_wl_init(term_t *t)
{
    /* v1: guarda nada — sem wl_data_device ainda */
    (void)t;
}

void clipboard_wl_offer(term_t *t, const char *mime, int fd)
{
    /* v1: "offer" = despeja memória interna no fd p/ o leitor Wayland */
    (void)mime;
    if (!t || fd < 0)
        return;
    if (!t->clipboard.is_set || !t->clipboard.data || t->clipboard.len == 0)
        return;
    size_t off = 0;
    while (off < t->clipboard.len) {
        ssize_t w = write(fd, t->clipboard.data + off,
                          t->clipboard.len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (w == 0)
            break;
        off += (size_t)w;
    }
}

void clipboard_wl_receive(term_t *t, const char *mime, int fd)
{
    /* v1: "receive" = lê tudo do fd p/ a memória interna */
    (void)mime;
    if (!t || fd < 0)
        return;
    size_t cap = 256;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return;
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                return;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;
            free(buf);
            return;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    clipboard_set(t, buf, len);
    free(buf);
}
