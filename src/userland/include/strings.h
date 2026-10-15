#ifndef STRINGS_H
#define STRINGS_H
#include <stddef.h>
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
void bzero(void *s, size_t n);
#endif