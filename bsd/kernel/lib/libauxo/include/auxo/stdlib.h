#ifndef AUXO_STDLIB_H
#define AUXO_STDLIB_H

#include <stdint.h>

__attribute__((noreturn))
void auxo_exit(int status);

int auxo_atoi(const char *s);
long auxo_strtol(const char *s, char **endptr, int base);
int auxo_abs(int value);

typedef struct {
    int quot;
    int rem;
} auxo_div_t;

auxo_div_t auxo_div(int numerator, int denominator);

#endif
