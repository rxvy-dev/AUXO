#ifndef AUXO_COOKIES_H
#define AUXO_COOKIES_H
/* the browser's cookie jar (net/cookies.c), kept in /home/root/.config/browser/cookies */
#include <stddef.h>
#include <stdint.h>
size_t   cookies_header(const char *host, const char *path, int secure, char *out, size_t cap);   /* "a=1; b=2" */
void     cookies_set(const char *host, const char *reqpath, int secure, const char *set_cookie);
void     cookies_save(void);              /* write the file if anything changed */
int      cookies_count(void);
void     cookies_clear(void);
uint64_t unix_now(void);
#endif
