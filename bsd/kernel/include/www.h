#ifndef AUXO_WWW_H
#define AUXO_WWW_H
/* the browser's loader thread (net/www.c): request a URL, poll state */
#include <stddef.h>
#include "net.h"

#define WWW_BUF (4u << 20)
enum { WWW_IDLE, WWW_REQUESTED, WWW_LOADING, WWW_DONE };

struct www {
    char url[512];
    char referer[512];
    char *post;                   /* body to POST (NULL: GET) */
    size_t post_len;
    volatile int state;
    volatile size_t bytes;        /* received so far, while loading */
    volatile int gen;             /* bumped after every load */
    struct http_result res;
    char *buf;
    size_t cap;
};
extern struct www www;

int  www_init(void);              /* allocates the 4 MiB buffer and starts the thread */
void www_request(const char *url);
void www_post(const char *url, const char *body, size_t len, const char *referer);   /* form submit */
void www_set_referer(const char *referer);

#endif
