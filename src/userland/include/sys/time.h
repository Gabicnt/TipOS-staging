#ifndef SYS_TIME_H
#define SYS_TIME_H
#include <stdint.h>
struct timeval { long tv_sec; long tv_usec; };
int gettimeofday(struct timeval *tv, void *timezone);
#endif