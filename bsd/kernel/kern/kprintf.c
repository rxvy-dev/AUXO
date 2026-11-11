/* Minimal printf: %d %i %u %x %X %p %s %c %% with -/0 flags, width, l/z length.
 * One formatter, two sinks: the console (kprintf) and a buffer (ksnprintf). */
#include "kern.h"
#include "arch.h"

struct sink {
    char  *buf;          /* NULL: write to the console */
    size_t cap, len;
};

static void emit(struct sink *s, char c)
{
    if (!s->buf) { console_putc(c); return; }
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void putnum(struct sink *s, unsigned long long v, unsigned base, int width, int zero, int left,
                   int neg, int upper)
{
    char buf[24];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = dig[v % base]; v /= base; }

    int len = n + neg;
    if (!zero && !left) while (width > len) { emit(s, ' '); width--; }
    if (neg) emit(s, '-');
    if (zero && !left) while (width > len) { emit(s, '0'); width--; }
    while (n) emit(s, buf[--n]);
    if (left) while (width > len) { emit(s, ' '); width--; }
}

static void format(struct sink *s, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { emit(s, *f); continue; }
        f++;
        int zero = 0, left = 0, width = 0, lng = 0;
        for (;; f++) {
            if (*f == '0') zero = 1;
            else if (*f == '-') left = 1;
            else break;
        }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        while (*f == 'l' || *f == 'z') { lng = 1; f++; }

        switch (*f) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            int neg = v < 0;
            putnum(s, neg ? 0ULL - (unsigned long long)v : (unsigned long long)v, 10, width, zero, left, neg, 0);
            break;
        }
        case 'u': {
            unsigned long long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            putnum(s, v, 10, width, zero, left, 0, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            putnum(s, v, 16, width, zero, left, 0, *f == 'X');
            break;
        }
        case 'p':
            emit(s, '0'); emit(s, 'x');
            putnum(s, (uintptr_t)va_arg(ap, void *), 16, 16, 1, 0, 0, 0);
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int l = (int)strlen(str);
            if (!left) while (width > l) { emit(s, ' '); width--; }
            while (*str) emit(s, *str++);
            if (left) while (width > l) { emit(s, ' '); width--; }
            break;
        }
        case 'c': emit(s, (char)va_arg(ap, int)); break;
        case '%': emit(s, '%'); break;
        case 0:   return;
        default:  emit(s, '%'); emit(s, *f); break;
        }
    }
}

int kvprintf(const char *fmt, va_list ap)
{
    struct sink s = { 0, 0, 0 };
    format(&s, fmt, ap);
    return 0;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = kvprintf(fmt, ap);
    va_end(ap);
    return r;
}

int ksnprintf(char *buf, size_t n, const char *fmt, ...)
{
    struct sink s = { buf, n, 0 };
    va_list ap;
    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);
    if (n) buf[s.len < n ? s.len : n - 1] = 0;
    return (int)s.len;
}

void panic(const char *fmt, ...)
{
    cli();
    kprintf("\n\033[0m\033[1;38;2;248;113;113m*** " AUXO_NAME " PANIC:\033[0m ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\nSystem halted.\n");
    for (;;) hlt();
}
