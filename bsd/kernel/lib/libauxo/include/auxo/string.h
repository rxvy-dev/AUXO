#ifndef AUXO_STRING_H
#define AUXO_STRING_H

#include <stdint.h>

uint64_t auxo_strlen(const char *s);
int auxo_strcmp(const char *a, const char *b);
int auxo_strncmp(const char *a, const char *b, uint64_t n);

void *auxo_memcpy(void *dest, const void *src, uint64_t n);
void *auxo_memmove(void *dest, const void *src, uint64_t n);
void *auxo_memset(void *dest, int value, uint64_t n);
int auxo_memcmp(const void *a, const void *b, uint64_t n);

#endif
