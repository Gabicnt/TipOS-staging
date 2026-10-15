/* ovsb.term — log.c — logging simples no stderr */
#include "term.h"

#include <stdio.h>
#include <stdarg.h>

/* verbose? (log_msg só imprime se true) */
static bool g_verbose = false;

void log_init(bool verbose)
{
    g_verbose = verbose;
}

void log_msg(const char *fmt, ...)
{
    va_list ap;

    if (!g_verbose || fmt == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void log_err(const char *fmt, ...)
{
    va_list ap;

    if (fmt == NULL)
        return;
    fputs("ovsb.term: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
