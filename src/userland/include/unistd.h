#ifndef UNISTD_H
#define UNISTD_H
#include <sys/types.h>
int close(int fd);
int read(int fd, void *buffer, int count);
int write(int fd, const void *buffer, int count);
int unlink(const char *path);
int rmdir(const char *path);
int isatty(int fd);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
#endif