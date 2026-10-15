/* ovsb.term — timer.c — timerfd CLOCK_MONOTONIC (moe moe kyun <3) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "term.h"
#include "timer.h"

#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#ifdef __linux__
#include <sys/timerfd.h>
#endif

int timer_create_ms(int ms, bool periodic)
{
#ifdef __linux__
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0)
        return -1; /* fallback: sem timer */
    timer_set(fd, ms, periodic);
    return fd;
#else
    (void)ms;
    (void)periodic;
    return -1;
#endif
}

void timer_set(int fd, int ms, bool periodic)
{
    if (fd < 0)
        return;
#ifdef __linux__
    struct itimerspec its;
    if (ms <= 0) {
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 0;
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
    } else {
        its.it_value.tv_sec = ms / 1000;
        its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
        if (periodic) {
            its.it_interval = its.it_value;
        } else {
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 0;
        }
    }
    /* ignora erro: fd pode ter sido fechado */
    (void)timerfd_settime(fd, 0, &its, NULL);
#else
    (void)ms;
    (void)periodic;
#endif
}

void timer_disarm(int fd)
{
    timer_set(fd, 0, false);
}
