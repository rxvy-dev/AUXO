#ifndef AUXO_STDIO_H
#define AUXO_STDIO_H

#include <stdint.h>
#include <stdarg.h>

int auxo_puts(const char *s);

int auxo_vsnprintf(char *buffer, uint64_t size,
                   const char *format, va_list args);

int auxo_snprintf(char *buffer, uint64_t size,
                  const char *format, ...);

int auxo_vprintf(const char *format, va_list args);
int auxo_printf(const char *format, ...);

#endif
