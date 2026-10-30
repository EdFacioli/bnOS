#ifndef BNOS_LIB_H
#define BNOS_LIB_H

#include <stddef.h>

/* Compiler may emit calls to these even in freestanding mode. */
void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

/* base 0 auto-detects a 0x/0X prefix (hex) or decimal. */
unsigned long strtoul(const char *s, char **end, int base);

#endif /* BNOS_LIB_H */
