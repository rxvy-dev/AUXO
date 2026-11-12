/* pkg: AuxoBSD's package manager.
 *
 * AuxoBSD runs unmodified Linux programs and its root filesystem is built from Ubuntu 24.04
 * packages, so the Ubuntu archive (tens of thousands of programs) is binary-compatible with it.
 * pkg reads the archive's package lists over HTTPS (BearSSL, Mozilla's roots), resolves
 * dependencies, checks every download against the SHA-256 in the list and unpacks the .deb
 * files itself (ar + tar, compressed with zstd, xz or gzip).
 *
 *   pkg update                  fetch the package lists
 *   pkg search WORD...          find packages
 *   pkg info NAME               about one package
 *   pkg install [-y] NAME...    install, with what it needs (NAME may be a ./file.deb)
 *   pkg remove NAME...          remove what pkg installed
 *   pkg upgrade [-y]            newer versions of what pkg installed
 *   pkg list [--all]            what's installed (--all: the base system too)
 *   pkg clean                   empty the download cache
 *
 * Maintainer scripts aren't run (there's no dpkg here); the common things they do are:
 * update-alternatives links and compiled GSettings schemas.
 *
 * Files: /etc/pkg/sources, /var/lib/pkg/{index,installed,assumed,files/NAME.list}, /var/cache/pkg.
 * PKG_ROOT=dir works on another root (for building images, and testing on a Linux host). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <lzma.h>
#include <zlib.h>
#include "bearssl.h"
#include "trust_anchors.h"

/* zstd: the runtime library is on every Ubuntu (and in AuxoBSD's root); its -dev package
 * often isn't, so the stable streaming API is declared here */
typedef struct ZSTD_DCtx_s ZSTD_DStream;
typedef struct { const void *src; size_t size, pos; } ZSTD_inBuffer;
typedef struct { void *dst; size_t size, pos; } ZSTD_outBuffer;
ZSTD_DStream *ZSTD_createDStream(void);
size_t ZSTD_freeDStream(ZSTD_DStream *);
size_t ZSTD_initDStream(ZSTD_DStream *);
size_t ZSTD_decompressStream(ZSTD_DStream *, ZSTD_outBuffer *, ZSTD_inBuffer *);
unsigned ZSTD_isError(size_t);
const char *ZSTD_getErrorName(size_t);

#define VERSION "1.0"
static char ROOT[512];
static int assume_yes, quiet_progress;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("pkg: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("pkg: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static char *xstrdup(const char *s) { char *r = strdup(s ? s : ""); if (!r) die("out of memory"); return r; }
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }

static const char *path(const char *rel)                 /* ROOT + rel, rotating buffers */
{
    static char bufs[8][1024];
    static int k;
    char *b = bufs[k++ & 7];
    snprintf(b, sizeof bufs[0], "%s%s", ROOT, rel);
    return b;
}

static void mkdirs(const char *p, int last)               /* mkdir -p; last: the whole path too */
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", p);
    size_t n = strlen(tmp);
    for (size_t i = 1; i < n; i++)
        if (tmp[i] == '/') { tmp[i] = 0; mkdir(tmp, 0755); tmp[i] = '/'; }
    if (last) mkdir(tmp, 0755);
}

static int read_file(const char *p, char **out, size_t *len)
{
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    size_t cap = 65536, n = 0;
    char *b = xmalloc(cap + 1);
    for (;;) {
        if (n == cap) { cap *= 2; b = xrealloc(b, cap + 1); }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    b[n] = 0;
    *out = b;
    if (len) *len = n;
    return 0;
}

static int write_file(const char *p, const void *d, size_t n)    /* atomically: tmp + rename */
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", p);
    mkdirs(p, 0);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const char *c = d;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        c += w; n -= (size_t)w;
    }
    close(fd);
    return rename(tmp, p);
}

static void human(double b, char *out, size_t n)
{
    if (b >= 1e9) snprintf(out, n, "%.1f GB", b / 1e9);
    else if (b >= 1e6) snprintf(out, n, "%.1f MB", b / 1e6);
    else if (b >= 1e3) snprintf(out, n, "%.0f kB", b / 1e3);
    else snprintf(out, n, "%.0f B", b);
}

/* ── Debian version comparison (as dpkg does it) ───────────────────────── */
static int order(int c)
{
    if (isdigit(c)) return 0;
    if (isalpha(c)) return c;
    if (c == '~') return -1;
    if (c) return c + 256;
    return 0;
}

static int verrevcmp(const char *a, const char *ae, const char *b, const char *be)
{
    while (a < ae || b < be) {
        int first = 0;
        while ((a < ae && !isdigit((unsigned char)*a)) || (b < be && !isdigit((unsigned char)*b))) {
            int ac = a < ae ? order((unsigned char)*a) : 0, bc = b < be ? order((unsigned char)*b) : 0;
            if (ac != bc) return ac - bc;
            a++; b++;
        }
        while (a < ae && *a == '0') a++;
        while (b < be && *b == '0') b++;
        while (a < ae && b < be && isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            if (!first) first = *a - *b;
            a++; b++;
        }
        if (a < ae && isdigit((unsigned char)*a)) return 1;
        if (b < be && isdigit((unsigned char)*b)) return -1;
        if (first) return first;
    }
    return 0;
}

static int vercmp(const char *a, const char *b)
{
    long ea = 0, eb = 0;
    const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
    if (ca) { ea = atol(a); a = ca + 1; }
    if (cb) { eb = atol(b); b = cb + 1; }
    if (ea != eb) return ea < eb ? -1 : 1;
    const char *ra = strrchr(a, '-'), *rb = strrchr(b, '-');
    const char *ae = ra ? ra : a + strlen(a), *be = rb ? rb : b + strlen(b);
    int r = verrevcmp(a, ae, b, be);
    if (r) return r;
    const char *rva = ra ? ra + 1 : "", *rvb = rb ? rb + 1 : "";
    return verrevcmp(rva, rva + strlen(rva), rvb, rvb + strlen(rvb));
}

/* ── HTTP(S) ───────────────────────────────────────────────────────────── */
struct conn {
    int fd, tls;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context io;
};

static int sock_read(void *ctx, unsigned char *b, size_t n)
{
    for (;;) {
        ssize_t r = read(*(int *)ctx, b, n);
        if (r < 0 && errno == EINTR) continue;
        return r <= 0 ? -1 : (int)r;
    }
}

static int sock_write(void *ctx, const unsigned char *b, size_t n)
{
    for (;;) {
        ssize_t r = write(*(int *)ctx, b, n);
        if (r < 0 && errno == EINTR) continue;
        return r <= 0 ? -1 : (int)r;
    }
}

static const char *tls_err(int e)
{
    switch (e) {
    case BR_ERR_X509_EXPIRED: return "the server's certificate has expired (or this computer's clock is wrong)";
    case BR_ERR_X509_NOT_TRUSTED: return "the server's certificate isn't from a trusted authority";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the certificate is for a different server";
    case BR_ERR_UNSUPPORTED_VERSION: return "the server only speaks TLS 1.3";
    default: return "the secure connection failed";
    }
}

struct url { int tls; char host[256]; char port[8]; char path[2048]; };

static int parse_url(const char *u, struct url *o)
{
    memset(o, 0, sizeof *o);
    if (!strncmp(u, "https://", 8)) { o->tls = 1; strcpy(o->port, "443"); u += 8; }
    else if (!strncmp(u, "http://", 7)) { strcpy(o->port, "80"); u += 7; }
    else return -1;
    size_t i = 0;
    while (*u && *u != '/' && *u != ':' && i + 1 < sizeof o->host) o->host[i++] = *u++;
    o->host[i] = 0;
    if (*u == ':') { u++; i = 0; while (isdigit((unsigned char)*u) && i + 1 < sizeof o->port) o->port[i++] = *u++; o->port[i] = 0; }
    snprintf(o->path, sizeof o->path, "%s", *u ? u : "/");
    return o->host[0] ? 0 : -1;
}

static int conn_open(struct conn *c, const struct url *u, char *err, size_t en)
{
    struct addrinfo hints = { 0 }, *res = 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(u->host, u->port, &hints, &res);
    if (r || !res) { snprintf(err, en, "can't find %s (%s)", u->host, gai_strerror(r)); return -1; }
    c->fd = -1;
    for (struct addrinfo *a = res; a && c->fd < 0; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) c->fd = fd; else close(fd);
    }
    freeaddrinfo(res);
    if (c->fd < 0) { snprintf(err, en, "can't connect to %s:%s (%s)", u->host, u->port, strerror(errno)); return -1; }
    c->tls = u->tls;
    if (c->tls) {
        br_ssl_client_init_full(&c->sc, &c->xc, TAs, TAs_NUM);
        br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
        unsigned char seed[48];
        if (getrandom(seed, sizeof seed, 0) != (ssize_t)sizeof seed) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            memcpy(seed, &ts, sizeof ts);
        }
        br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof seed);
        time_t now = time(0);
        br_x509_minimal_set_time(&c->xc, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
        br_ssl_client_reset(&c->sc, u->host, 0);
        br_sslio_init(&c->io, &c->sc.eng, sock_read, &c->fd, sock_write, &c->fd);
    }
    return 0;
}

static int conn_write(struct conn *c, const void *b, size_t n)
{
    if (c->tls) return br_sslio_write_all(&c->io, b, n) < 0 || br_sslio_flush(&c->io) < 0 ? -1 : 0;
    const char *p = b;
    while (n) { int w = sock_write(&c->fd, (const unsigned char *)p, n); if (w <= 0) return -1; p += w; n -= (size_t)w; }
    return 0;
}

static int conn_read(struct conn *c, void *b, size_t n)
{
    if (c->tls) return br_sslio_read(&c->io, b, n);
    return sock_read(&c->fd, b, n);
}

static void conn_close(struct conn *c)
{
    if (c->tls) br_sslio_close(&c->io);
    close(c->fd);
}

typedef int (*sink_fn)(void *arg, const void *data, size_t n);

static double t_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void progress(const char *label, uint64_t got, uint64_t total, int done)
{
    static double last;
    double t = t_now();
    if (quiet_progress || (!done && t - last < 0.25)) return;
    last = t;
    char a[32], b[32];
    human((double)got, a, sizeof a);
    if (total) { human((double)total, b, sizeof b); fprintf(stderr, "\r  %-40.40s %9s / %-9s %3d%%", label, a, b, (int)(got * 100 / total)); }
    else fprintf(stderr, "\r  %-40.40s %9s", label, a);
    if (done) fputc('\n', stderr);
    fflush(stderr);
}

/* GET url, body to sink; follows redirects. 0 or -1 (err filled in) */
static int http_get(const char *url0, sink_fn sink, void *arg, const char *label, char *err, size_t en)
{
    char url[2048];
    snprintf(url, sizeof url, "%s", url0);
    static struct conn c;
    for (int hop = 0; hop < 6; hop++) {
        struct url u;
        if (parse_url(url, &u) < 0) { snprintf(err, en, "not an http(s) address: %s", url); return -1; }
        if (conn_open(&c, &u, err, en) < 0) return -1;
        char req[3072];
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: pkg/" VERSION " (AuxoBSD)\r\n"
                          "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", u.path, u.host);
        if (conn_write(&c, req, (size_t)rl) < 0) {
            snprintf(err, en, "%s", c.tls ? tls_err(br_ssl_engine_last_error(&c.sc.eng)) : "the connection dropped");
            conn_close(&c);
            return -1;
        }
        /* headers */
        static char hb[32768];
        size_t hn = 0;
        char *body = 0;
        while (!body && hn < sizeof hb - 1) {
            int r = conn_read(&c, hb + hn, sizeof hb - 1 - hn);
            if (r <= 0) break;
            hn += (size_t)r;
            hb[hn] = 0;
            char *e = strstr(hb, "\r\n\r\n");
            if (e) body = e + 4;
        }
        if (!body) {
            if (c.tls && br_ssl_engine_last_error(&c.sc.eng)) snprintf(err, en, "%s: %s", u.host, tls_err(br_ssl_engine_last_error(&c.sc.eng)));
            else snprintf(err, en, "%s didn't answer properly", u.host);
            conn_close(&c);
            return -1;
        }
        int status = atoi(strchr(hb, ' ') ? strchr(hb, ' ') + 1 : "0");
        char location[2048] = "";
        uint64_t clen = 0;
        int chunked = 0;
        for (char *line = strstr(hb, "\r\n") + 2; line < body - 2; ) {
            char *nl = strstr(line, "\r\n");
            if (!nl) break;
            *nl = 0;
            if (!strncasecmp(line, "location:", 9)) { char *v = line + 9; while (*v == ' ') v++; snprintf(location, sizeof location, "%s", v); }
            else if (!strncasecmp(line, "content-length:", 15)) clen = strtoull(line + 15, 0, 10);
            else if (!strncasecmp(line, "transfer-encoding:", 18) && strcasestr(line, "chunked")) chunked = 1;
            line = nl + 2;
        }
        if (status >= 300 && status < 400 && location[0]) {
            conn_close(&c);
            if (location[0] == '/') { char n2[2048]; snprintf(n2, sizeof n2, "%s://%s:%s%s", u.tls ? "https" : "http", u.host, u.port, location); snprintf(url, sizeof url, "%s", n2); }
            else snprintf(url, sizeof url, "%s", location);
            continue;
        }
        if (status != 200) { snprintf(err, en, "%s: HTTP %d", url, status); conn_close(&c); return -1; }
        /* body */
        size_t have = hn - (size_t)(body - hb);
        static char buf[65536];
        memmove(buf, body, have);
        uint64_t got = 0;
        int ok = 0;
        if (!chunked) {
            for (;;) {
                if (have) {
                    if (sink(arg, buf, have) < 0) { snprintf(err, en, "couldn't save the download"); conn_close(&c); return -1; }
                    got += have;
                    progress(label, got, clen, 0);
                    have = 0;
                }
                if (clen && got >= clen) { ok = 1; break; }
                int r = conn_read(&c, buf, sizeof buf);
                if (r <= 0) { ok = !clen; break; }
                have = (size_t)r;
            }
        } else {
            /* chunked: [hex size CRLF data CRLF]* 0 CRLF */
            static char cb[65536 + 64];
            size_t cn = have;
            memcpy(cb, buf, have);
            for (;;) {
                char *crlf = memmem(cb, cn, "\r\n", 2);
                while (!crlf && cn < 64) { int r = conn_read(&c, cb + cn, sizeof cb - cn); if (r <= 0) break; cn += (size_t)r; crlf = memmem(cb, cn, "\r\n", 2); }
                if (!crlf) break;
                uint64_t sz = strtoull(cb, 0, 16);
                size_t hl = (size_t)(crlf - cb) + 2;
                memmove(cb, cb + hl, cn - hl); cn -= hl;
                if (!sz) { ok = 1; break; }
                while (sz) {
                    if (!cn) { int r = conn_read(&c, cb, 65536); if (r <= 0) break; cn = (size_t)r; }
                    size_t take = cn < sz ? cn : (size_t)sz;
                    if (sink(arg, cb, take) < 0) { snprintf(err, en, "couldn't save the download"); conn_close(&c); return -1; }
                    got += take; sz -= take;
                    memmove(cb, cb + take, cn - take); cn -= take;
                    progress(label, got, 0, 0);
                }
                if (sz) break;
                while (cn < 2) { int r = conn_read(&c, cb + cn, sizeof cb - cn); if (r <= 0) break; cn += (size_t)r; }
                if (cn < 2) break;
                memmove(cb, cb + 2, cn - 2); cn -= 2;
            }
        }
        conn_close(&c);
        progress(label, got, clen, 1);
        if (!ok) { snprintf(err, en, "the download from %s stopped early", u.host); return -1; }
        return 0;
    }
    snprintf(err, en, "too many redirects");
    return -1;
}

/* ── decompression ─────────────────────────────────────────────────────── */
enum { C_NONE, C_GZ, C_XZ, C_ZST };

static int comp_of(const char *name)
{
    size_t n = strlen(name);
    if (n > 3 && !strcmp(name + n - 3, ".gz")) return C_GZ;
    if (n > 3 && !strcmp(name + n - 3, ".xz")) return C_XZ;
    if (n > 4 && !strcmp(name + n - 4, ".zst")) return C_ZST;
    return C_NONE;
}

struct dec {
    int type, err, done;
    z_stream z;
    lzma_stream x;
    ZSTD_DStream *zs;
    sink_fn out;
    void *arg;
};

static void dec_init(struct dec *d, int type, sink_fn out, void *arg)
{
    memset(d, 0, sizeof *d);
    d->type = type; d->out = out; d->arg = arg;
    if (type == C_GZ && inflateInit2(&d->z, 16 + MAX_WBITS) != Z_OK) d->err = 1;
    if (type == C_XZ) { lzma_stream init = LZMA_STREAM_INIT; d->x = init; if (lzma_stream_decoder(&d->x, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) d->err = 1; }
    if (type == C_ZST) { d->zs = ZSTD_createDStream(); if (!d->zs || ZSTD_isError(ZSTD_initDStream(d->zs))) d->err = 1; }
}

static int dec_feed(void *arg, const void *data, size_t n)
{
    struct dec *d = arg;
    static unsigned char ob[262144];
    if (d->err) return -1;
    if (d->type == C_NONE) return d->out(d->arg, data, n);
    if (d->type == C_GZ) {
        d->z.next_in = (unsigned char *)data; d->z.avail_in = (unsigned)n;
        while (d->z.avail_in && !d->done) {
            d->z.next_out = ob; d->z.avail_out = sizeof ob;
            int r = inflate(&d->z, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) { d->err = 1; return -1; }
            if (d->out(d->arg, ob, sizeof ob - d->z.avail_out) < 0) { d->err = 1; return -1; }
            if (r == Z_STREAM_END) {                     /* gzip members can be concatenated */
                if (d->z.avail_in) inflateReset(&d->z); else d->done = 1;
            }
        }
        return 0;
    }
    if (d->type == C_XZ) {
        d->x.next_in = data; d->x.avail_in = n;
        while (d->x.avail_in) {
            d->x.next_out = ob; d->x.avail_out = sizeof ob;
            lzma_ret r = lzma_code(&d->x, LZMA_RUN);
            if (r != LZMA_OK && r != LZMA_STREAM_END) { d->err = 1; return -1; }
            if (d->out(d->arg, ob, sizeof ob - d->x.avail_out) < 0) { d->err = 1; return -1; }
            if (r == LZMA_STREAM_END) break;
        }
        return 0;
    }
    ZSTD_inBuffer in = { data, n, 0 };
    while (in.pos < in.size) {
        ZSTD_outBuffer o = { ob, sizeof ob, 0 };
        size_t r = ZSTD_decompressStream(d->zs, &o, &in);
        if (ZSTD_isError(r)) { d->err = 1; return -1; }
        if (o.pos && d->out(d->arg, ob, o.pos) < 0) { d->err = 1; return -1; }
    }
    return 0;
}

static int dec_finish(struct dec *d)
{
    static unsigned char ob[262144];
    if (d->type == C_XZ && !d->err) {
        for (;;) {
            d->x.next_out = ob; d->x.avail_out = sizeof ob;
            lzma_ret r = lzma_code(&d->x, LZMA_FINISH);
            if (r != LZMA_OK && r != LZMA_STREAM_END) { d->err = 1; break; }
            if (d->out(d->arg, ob, sizeof ob - d->x.avail_out) < 0) { d->err = 1; break; }
            if (r == LZMA_STREAM_END) break;
        }
    }
    if (d->type == C_ZST && !d->err) {                   /* flush what's buffered */
        for (int k = 0; k < 64; k++) {
            ZSTD_inBuffer in = { "", 0, 0 };
            ZSTD_outBuffer o = { ob, sizeof ob, 0 };
            size_t r = ZSTD_decompressStream(d->zs, &o, &in);
            if (ZSTD_isError(r)) { d->err = 1; break; }
            if (o.pos && d->out(d->arg, ob, o.pos) < 0) { d->err = 1; break; }
            if (!o.pos) break;
        }
    }
    if (d->type == C_GZ) inflateEnd(&d->z);
    if (d->type == C_XZ) lzma_end(&d->x);
    if (d->type == C_ZST) ZSTD_freeDStream(d->zs);
    return d->err ? -1 : 0;
}

/* ── the package index ─────────────────────────────────────────────────── */
struct pkg {
    char *name, *version, *filename, *sha256, *depends, *predepends, *provides, *desc, *section;
    uint64_t size, isize;
    int src;                       /* which source (mirror) it came from */
    struct pkg *hnext;
};

#define HBITS 17
static struct pkg *htab[1 << HBITS];
static struct pkg **plist;
static size_t npkg, cappkg;
static char **mirrors;
static int nmirrors;

static uint32_t hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h & ((1u << HBITS) - 1);
}

static struct pkg *find_n(const char *name, size_t n)
{
    for (struct pkg *p = htab[hash(name, n)]; p; p = p->hnext)
        if (strlen(p->name) == n && !memcmp(p->name, name, n)) return p;
    return 0;
}
static struct pkg *find(const char *name) { return find_n(name, strlen(name)); }

static void add_pkg(struct pkg *p)
{
    struct pkg *old = find(p->name);
    if (old) {                                    /* keep the newer version */
        if (vercmp(p->version, old->version) <= 0) return;
        struct pkg *keep_next = old->hnext;
        *old = *p;
        old->hnext = keep_next;
        return;
    }
    struct pkg *n = xmalloc(sizeof *n);
    *n = *p;
    uint32_t h = hash(n->name, strlen(n->name));
    n->hnext = htab[h];
    htab[h] = n;
    if (npkg == cappkg) { cappkg = cappkg ? cappkg * 2 : 8192; plist = xrealloc(plist, cappkg * sizeof *plist); }
    plist[npkg++] = n;
}

static int index_load(int need)
{
    static int loaded;
    if (loaded) return 0;
    char *d; size_t n;
    if (read_file(path("/var/lib/pkg/index"), &d, &n) < 0) {
        if (need) die("there's no package list yet: run `pkg update` first");
        return -1;
    }
    loaded = 1;
    /* line 1..k: "mirror\t<url>"; then one package per line, fields separated by \t */
    for (char *line = d; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "mirror\t", 7)) {
            mirrors = xrealloc(mirrors, (size_t)(nmirrors + 1) * sizeof *mirrors);
            mirrors[nmirrors++] = line + 7;
        } else {
            char *f[12];
            int k = 0;
            f[k++] = line;
            for (char *p = line; *p && k < 12; p++) if (*p == '\t') { *p = 0; f[k++] = p + 1; }
            if (k == 12) {
                struct pkg p = { 0 };
                p.name = f[0]; p.version = f[1]; p.src = atoi(f[2]); p.filename = f[3]; p.size = strtoull(f[4], 0, 10);
                p.sha256 = f[5]; p.isize = strtoull(f[6], 0, 10); p.depends = f[7]; p.predepends = f[8]; p.provides = f[9];
                p.section = f[10]; p.desc = f[11];
                add_pkg(&p);
            }
        }
        line = nl ? nl + 1 : 0;
    }
    return 0;
}

/* ── what's installed ──────────────────────────────────────────────────── */
struct inst { char *name, *version; int base; struct inst *next; };
static struct inst *installed;
static char **assumed;
static int nassumed;

static struct inst *inst_find(const char *name)
{
    for (struct inst *i = installed; i; i = i->next) if (!strcmp(i->name, name)) return i;
    return 0;
}

static void inst_load(void)
{
    static int done;
    if (done) return;
    done = 1;
    char *d;
    if (read_file(path("/var/lib/pkg/installed"), &d, 0) == 0)
        for (char *line = strtok(d, "\n"); line; line = strtok(0, "\n")) {
            char name[256], ver[256], how[16] = "pkg";
            if (sscanf(line, "%255s %255s %15s", name, ver, how) < 2) continue;
            struct inst *i = xmalloc(sizeof *i);
            i->name = xstrdup(name); i->version = xstrdup(ver); i->base = !strcmp(how, "base");
            i->next = installed; installed = i;
        }
    if (read_file(path("/var/lib/pkg/assumed"), &d, 0) == 0)
        for (char *line = strtok(d, "\n"); line; line = strtok(0, "\n")) {
            assumed = xrealloc(assumed, (size_t)(nassumed + 1) * sizeof *assumed);
            assumed[nassumed++] = line;
        }
}

static void inst_save(void)
{
    size_t cap = 4096, n = 0;
    char *b = xmalloc(cap);
    for (struct inst *i = installed; i; i = i->next) {
        size_t need = strlen(i->name) + strlen(i->version) + 16;
        if (n + need >= cap) { cap = cap * 2 + need; b = xrealloc(b, cap); }
        n += (size_t)snprintf(b + n, cap - n, "%s %s %s\n", i->name, i->version, i->base ? "base" : "pkg");
    }
    if (write_file(path("/var/lib/pkg/installed"), b, n) < 0) die("can't write %s: %s", path("/var/lib/pkg/installed"), strerror(errno));
    free(b);
}

static int is_assumed(const char *name)
{
    for (int i = 0; i < nassumed; i++) if (!strcmp(assumed[i], name)) return 1;
    return 0;
}

/* does something installed provide this name (a virtual package)? */
static int provided_by_installed(const char *name)
{
    size_t nl = strlen(name);
    for (struct inst *i = installed; i; i = i->next) {
        struct pkg *p = find(i->name);
        if (!p || !p->provides[0]) continue;
        for (const char *s = p->provides; *s; ) {
            while (*s == ' ' || *s == ',') s++;
            const char *e = s;
            while (*e && *e != ',' && *e != ' ' && *e != '(') e++;
            if ((size_t)(e - s) == nl && !memcmp(s, name, nl)) return 1;
            while (*e && *e != ',') e++;
            s = e;
        }
    }
    return 0;
}

/* ── pkg update ────────────────────────────────────────────────────────── */
struct stanza_parser {
    char *buf; size_t n, cap;            /* the current line(s) */
    struct pkg cur;
    char *fields[11];
    int src;
    size_t count;
};

static char *field_dup(const char *v) { while (*v == ' ') v++; return xstrdup(v); }

static void stanza_end(struct stanza_parser *sp)
{
    struct pkg *c = &sp->cur;
    if (c->name && c->version && c->filename && c->sha256) {
        if (!c->depends) c->depends = xstrdup("");
        if (!c->predepends) c->predepends = xstrdup("");
        if (!c->provides) c->provides = xstrdup("");
        if (!c->desc) c->desc = xstrdup("");
        if (!c->section) c->section = xstrdup("");
        c->src = sp->src;
        add_pkg(c);
        sp->count++;
    }
    memset(c, 0, sizeof *c);
}

static void stanza_line(struct stanza_parser *sp, char *l)
{
    struct pkg *c = &sp->cur;
    if (!*l) { stanza_end(sp); return; }
    if (*l == ' ') return;                                 /* continuation (long description) */
    for (char *p = l; *p; p++) if (*p == '\t') *p = ' ';
    char *colon = strchr(l, ':');
    if (!colon) return;
    *colon = 0;
    const char *v = colon + 1;
    if (!strcmp(l, "Package")) c->name = field_dup(v);
    else if (!strcmp(l, "Version")) c->version = field_dup(v);
    else if (!strcmp(l, "Filename")) c->filename = field_dup(v);
    else if (!strcmp(l, "SHA256")) c->sha256 = field_dup(v);
    else if (!strcmp(l, "Size")) c->size = strtoull(v, 0, 10);
    else if (!strcmp(l, "Installed-Size")) c->isize = strtoull(v, 0, 10) * 1024;
    else if (!strcmp(l, "Depends")) c->depends = field_dup(v);
    else if (!strcmp(l, "Pre-Depends")) c->predepends = field_dup(v);
    else if (!strcmp(l, "Provides")) c->provides = field_dup(v);
    else if (!strcmp(l, "Description")) c->desc = field_dup(v);
    else if (!strcmp(l, "Section")) c->section = field_dup(v);
}

static int stanza_feed(void *arg, const void *data, size_t n)
{
    struct stanza_parser *sp = arg;
    const char *d = data;
    for (size_t i = 0; i < n; i++) {
        if (d[i] == '\n') {
            if (sp->n == sp->cap) { sp->cap = sp->cap ? sp->cap * 2 : 4096; sp->buf = xrealloc(sp->buf, sp->cap); }
            sp->buf[sp->n] = 0;
            stanza_line(sp, sp->buf);
            sp->n = 0;
            continue;
        }
        if (sp->n + 1 >= sp->cap) { sp->cap = sp->cap ? sp->cap * 2 : 4096; sp->buf = xrealloc(sp->buf, sp->cap); }
        sp->buf[sp->n++] = d[i];
    }
    return 0;
}

static void default_sources(void)
{
    static const char def[] =
        "# Where pkg gets packages: MIRROR SUITE COMPONENT...\n"
        "# AuxoBSD's root is built from Ubuntu 24.04 (noble), so these match it.\n"
        "https://archive.ubuntu.com/ubuntu noble main universe\n"
        "https://archive.ubuntu.com/ubuntu noble-updates main universe\n"
        "https://archive.ubuntu.com/ubuntu noble-security main universe\n";
    struct stat st;
    if (stat(path("/etc/pkg/sources"), &st) < 0) write_file(path("/etc/pkg/sources"), def, sizeof def - 1);
}

static int cmd_update(void)
{
    default_sources();
    char *conf;
    if (read_file(path("/etc/pkg/sources"), &conf, 0) < 0) die("can't read /etc/pkg/sources");
    char *mirror_list[32];
    int nm = 0, failures = 0, lists = 0;
    for (char *line = strtok(conf, "\n"); line; line = strtok(0, "\n")) {
        while (*line == ' ') line++;
        if (!*line || *line == '#') continue;
        char *save, *mirror = strtok_r(line, " \t", &save), *suite = strtok_r(0, " \t", &save);
        if (!mirror || !suite) continue;
        size_t ml = strlen(mirror);
        while (ml && mirror[ml - 1] == '/') mirror[--ml] = 0;
        int src = -1;
        for (int i = 0; i < nm; i++) if (!strcmp(mirror_list[i], mirror)) src = i;
        if (src < 0 && nm < 32) { mirror_list[nm] = mirror; src = nm++; }
        for (char *comp; (comp = strtok_r(0, " \t", &save)); ) {
            char url[2048], label[128], err[512];
            snprintf(url, sizeof url, "%s/dists/%s/%s/binary-amd64/Packages.gz", mirror, suite, comp);
            snprintf(label, sizeof label, "%s/%s", suite, comp);
            struct stanza_parser sp = { 0 };
            sp.src = src;
            struct dec d;
            dec_init(&d, C_GZ, stanza_feed, &sp);
            int r = http_get(url, dec_feed, &d, label, err, sizeof err);
            if (dec_finish(&d) < 0 && r == 0) { snprintf(err, sizeof err, "the list is damaged"); r = -1; }
            stanza_line(&sp, (char[]){ 0 });
            if (r < 0) { warn("%s: %s", label, err); failures++; }
            else lists++;
            free(sp.buf);
        }
    }
    if (!lists) die("couldn't fetch any package list (is the network up? `ifconfig`)");
    /* write the index */
    size_t cap = 1 << 24, n = 0;
    char *b = xmalloc(cap);
    for (int i = 0; i < nm; i++) n += (size_t)snprintf(b + n, cap - n, "mirror\t%s\n", mirror_list[i]);
    for (size_t i = 0; i < npkg; i++) {
        struct pkg *p = plist[i];
        size_t need = strlen(p->name) + strlen(p->version) + strlen(p->filename) + strlen(p->sha256) + strlen(p->depends) +
                      strlen(p->predepends) + strlen(p->provides) + strlen(p->section) + strlen(p->desc) + 80;
        if (n + need >= cap) { cap = cap * 2 + need; b = xrealloc(b, cap); }
        n += (size_t)snprintf(b + n, cap - n, "%s\t%s\t%d\t%s\t%llu\t%s\t%llu\t%s\t%s\t%s\t%s\t%s\n", p->name, p->version, p->src,
                              p->filename, (unsigned long long)p->size, p->sha256, (unsigned long long)p->isize, p->depends,
                              p->predepends, p->provides, p->section, p->desc);
    }
    if (write_file(path("/var/lib/pkg/index"), b, n) < 0) die("can't write the package list: %s", strerror(errno));
    printf("%zu packages available%s\n", npkg, failures ? " (some lists failed: see above)" : "");
    return failures ? 1 : 0;
}

/* ── dependencies ──────────────────────────────────────────────────────── */
struct plan { struct pkg **p; int n, cap; };

static int in_plan(struct plan *pl, const char *name)
{
    for (int i = 0; i < pl->n; i++) if (!strcmp(pl->p[i]->name, name)) return 1;
    return 0;
}

static int plan_provides(struct plan *pl, const char *name)
{
    size_t nl = strlen(name);
    for (int i = 0; i < pl->n; i++)
        for (const char *s = pl->p[i]->provides; *s; ) {
            while (*s == ' ' || *s == ',') s++;
            const char *e = s;
            while (*e && *e != ',' && *e != ' ' && *e != '(') e++;
            if ((size_t)(e - s) == nl && !memcmp(s, name, nl)) return 1;
            while (*e && *e != ',') e++;
            s = e;
        }
    return 0;
}

static int satisfied(struct plan *pl, const char *name)
{
    return inst_find(name) || is_assumed(name) || in_plan(pl, name) || provided_by_installed(name) || plan_provides(pl, name);
}

/* a package that provides the virtual name (the first one, preferring ones already planned) */
static struct pkg *provider(const char *name)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i < npkg; i++)
        for (const char *s = plist[i]->provides; *s; ) {
            while (*s == ' ' || *s == ',') s++;
            const char *e = s;
            while (*e && *e != ',' && *e != ' ' && *e != '(') e++;
            if ((size_t)(e - s) == nl && !memcmp(s, name, nl)) return plist[i];
            while (*e && *e != ',') e++;
            s = e;
        }
    return 0;
}

static int plan_add(struct plan *pl, struct pkg *p, int depth);

/* "a (>= 1), b | c:any, d [amd64]" */
static int resolve_list(struct plan *pl, const char *deps, const char *who, int depth)
{
    int ok = 1;
    char *copy = xstrdup(deps);
    for (char *grp = copy; grp && *grp; ) {
        char *comma = strchr(grp, ',');
        if (comma) *comma = 0;
        /* alternatives */
        char *alts[16];
        int na = 0;
        for (char *a = grp; a && na < 16; ) {
            char *bar = strchr(a, '|');
            if (bar) *bar = 0;
            while (*a == ' ') a++;
            char *e = a;
            while (*e && *e != ' ' && *e != '(' && *e != '[' && *e != ':') e++;
            *e = 0;
            if (*a) alts[na++] = a;
            a = bar ? bar + 1 : 0;
        }
        int done = 0;
        for (int i = 0; i < na && !done; i++) if (satisfied(pl, alts[i])) done = 1;
        for (int i = 0; i < na && !done; i++) {
            struct pkg *p = find(alts[i]);
            if (!p) p = provider(alts[i]);
            if (p && plan_add(pl, p, depth + 1) == 0) done = 1;
        }
        if (!done && na) { warn("%s needs %s, which isn't available: skipping that", who, alts[0]); ok = 0; }
        grp = comma ? comma + 1 : 0;
    }
    free(copy);
    return ok ? 0 : -1;
}

static int plan_add(struct plan *pl, struct pkg *p, int depth)
{
    if (in_plan(pl, p->name)) return 0;
    if (depth > 64) return -1;
    if (pl->n == pl->cap) { pl->cap = pl->cap ? pl->cap * 2 : 64; pl->p = xrealloc(pl->p, (size_t)pl->cap * sizeof *pl->p); }
    pl->p[pl->n++] = p;
    resolve_list(pl, p->predepends, p->name, depth);
    resolve_list(pl, p->depends, p->name, depth);
    return 0;
}

/* ── unpacking .deb files ──────────────────────────────────────────────── */
struct tarx {
    unsigned char hdr[512];
    size_t hn;
    uint64_t left, pad;
    int fd;
    char longname[4096], longlink[4096], paxpath[4096], paxlink[4096];
    char *meta; size_t metan, metacap; int meta_kind;    /* collecting an L/K/x entry */
    char **files; int nfiles, capfiles;
    int capture;                                          /* control.tar: keep postinst */
    char *postinst; size_t postn;
    int capturing;
};

static uint64_t octal(const unsigned char *s, size_t n)
{
    if (s[0] & 0x80) {                                   /* GNU base-256 */
        uint64_t v = 0;
        for (size_t i = 1; i < n; i++) v = v << 8 | s[i];
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i]; i++) if (s[i] >= '0' && s[i] <= '7') v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

static void tar_remember(struct tarx *t, const char *rel)
{
    if (t->nfiles == t->capfiles) { t->capfiles = t->capfiles ? t->capfiles * 2 : 256; t->files = xrealloc(t->files, (size_t)t->capfiles * sizeof *t->files); }
    t->files[t->nfiles++] = xstrdup(rel);
}

static void pax_parse(struct tarx *t)
{
    for (size_t i = 0; i < t->metan; ) {
        size_t len = (size_t)atol(t->meta + i);
        if (!len || i + len > t->metan) break;
        char *rec = t->meta + i, *sp = memchr(rec, ' ', len), *eq = sp ? memchr(sp, '=', len - (size_t)(sp - rec)) : 0;
        if (sp && eq) {
            size_t vl = len - (size_t)(eq + 1 - rec) - 1;
            if (!strncmp(sp + 1, "path=", 5)) { snprintf(t->paxpath, sizeof t->paxpath, "%.*s", (int)vl, eq + 1); }
            else if (!strncmp(sp + 1, "linkpath=", 9)) { snprintf(t->paxlink, sizeof t->paxlink, "%.*s", (int)vl, eq + 1); }
        }
        i += len;
    }
}

static void tar_entry(struct tarx *t)
{
    const unsigned char *h = t->hdr;
    char name[4200], lnk[4200];
    char type = (char)h[156];
    uint64_t size = octal(h + 124, 12);
    if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
        t->meta_kind = type;
        t->metan = 0;
        t->left = size; t->pad = (512 - size % 512) % 512;
        t->fd = -2;
        return;
    }
    if (t->paxpath[0]) snprintf(name, sizeof name, "%s", t->paxpath);
    else if (t->longname[0]) snprintf(name, sizeof name, "%s", t->longname);
    else if (!memcmp(h + 257, "ustar", 5) && h[345]) snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
    else snprintf(name, sizeof name, "%.100s", (const char *)h);
    if (t->paxlink[0]) snprintf(lnk, sizeof lnk, "%s", t->paxlink);
    else if (t->longlink[0]) snprintf(lnk, sizeof lnk, "%s", t->longlink);
    else snprintf(lnk, sizeof lnk, "%.100s", (const char *)h + 157);
    t->paxpath[0] = t->paxlink[0] = t->longname[0] = t->longlink[0] = 0;
    t->left = size; t->pad = (512 - size % 512) % 512;
    t->fd = -1;

    const char *rel = name;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    while (*rel == '/') rel++;
    size_t rl = strlen(rel);
    while (rl && rel[rl - 1] == '/') ((char *)rel)[--rl] = 0;
    if (!rl || strstr(rel, "../")) return;

    if (t->capture) {                                    /* control.tar */
        if (!strcmp(rel, "postinst") && (type == '0' || type == 0)) { t->capturing = 1; t->postn = 0; free(t->postinst); t->postinst = xmalloc(size + 1); }
        return;
    }
    char full[4300];
    snprintf(full, sizeof full, "%s/%s", ROOT, rel);
    mode_t mode = (mode_t)(octal(h + 100, 8) & 07777);
    if (type == '5') { mkdirs(full, 1); chmod(full, mode | 0700); return; }
    mkdirs(full, 0);
    struct stat st;
    if (lstat(full, &st) == 0 && !S_ISDIR(st.st_mode)) unlink(full);
    char relpath[4300];
    snprintf(relpath, sizeof relpath, "/%s", rel);
    if (type == '2') {
        if (symlink(lnk, full) == 0) tar_remember(t, relpath);
        return;
    }
    if (type == '1') {
        char target[4300];
        const char *lr = lnk;
        while (lr[0] == '.' && lr[1] == '/') lr += 2;
        snprintf(target, sizeof target, "%s/%s", ROOT, lr);
        if (link(target, full) == 0) { tar_remember(t, relpath); return; }
        char *d; size_t n;                               /* no hard links: copy */
        if (read_file(target, &d, &n) == 0) { write_file(full, d, n); chmod(full, 0755); free(d); tar_remember(t, relpath); }
        return;
    }
    if (type == '0' || type == 0 || type == '7') {
        if (S_ISDIR(st.st_mode) && lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) return;
        t->fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
        if (t->fd < 0) warn("can't write %s: %s", relpath, strerror(errno));
        else { fchmod(t->fd, mode ? mode : 0644); tar_remember(t, relpath); }
    }
}

static int tar_feed(void *arg, const void *data, size_t n)
{
    struct tarx *t = arg;
    const unsigned char *d = data;
    while (n) {
        if (t->left) {
            size_t take = n < t->left ? n : (size_t)t->left;
            if (t->fd == -2) {                           /* metadata entry */
                if (t->metan + take + 1 > t->metacap) { t->metacap = (t->metan + take + 1) * 2; t->meta = xrealloc(t->meta, t->metacap); }
                memcpy(t->meta + t->metan, d, take); t->metan += take;
            } else if (t->fd >= 0) {
                const unsigned char *p = d;
                size_t k = take;
                while (k) { ssize_t w = write(t->fd, p, k); if (w < 0 && errno == EINTR) continue; if (w <= 0) { warn("write failed: %s", strerror(errno)); return -1; } p += w; k -= (size_t)w; }
            } else if (t->capturing) { memcpy(t->postinst + t->postn, d, take); t->postn += take; }
            d += take; n -= take; t->left -= take;
            if (!t->left) {
                if (t->fd >= 0) { close(t->fd); t->fd = -1; }
                if (t->capturing) { t->postinst[t->postn] = 0; t->capturing = 0; }
                if (t->fd == -2) {
                    t->meta[t->metan] = 0;
                    if (t->meta_kind == 'L') snprintf(t->longname, sizeof t->longname, "%s", t->meta);
                    else if (t->meta_kind == 'K') snprintf(t->longlink, sizeof t->longlink, "%s", t->meta);
                    else if (t->meta_kind == 'x') pax_parse(t);
                    t->fd = -1;
                }
            }
            continue;
        }
        if (t->pad) { size_t take = n < t->pad ? n : (size_t)t->pad; d += take; n -= take; t->pad -= take; continue; }
        size_t take = 512 - t->hn < n ? 512 - t->hn : n;
        memcpy(t->hdr + t->hn, d, take);
        t->hn += take; d += take; n -= take;
        if (t->hn == 512) {
            t->hn = 0;
            int zero = 1;
            for (int i = 0; i < 512; i++) if (t->hdr[i]) { zero = 0; break; }
            if (!zero) tar_entry(t);
        }
    }
    return 0;
}

/* update-alternatives --install LINK NAME PATH PRIO [--slave LINK NAME PATH]...
 * (the links made are added to the package's file list, so `pkg remove` takes them away) */
static char *alt_links[64];
static int nalt;
static void alternatives(const char *postinst)
{
    nalt = 0;
    if (!postinst) return;
    const char *s = postinst;
    while ((s = strstr(s, "update-alternatives"))) {
        const char *e = s;
        while (*e && *e != '\n') { if (e[0] == '\\' && e[1] == '\n') e += 2; else e++; }
        char line[2048];
        size_t l = (size_t)(e - s) < sizeof line - 1 ? (size_t)(e - s) : sizeof line - 1;
        memcpy(line, s, l); line[l] = 0;
        for (char *p = line; *p; p++) if (*p == '\\' || *p == '\n' || *p == '\t') *p = ' ';
        s = e;
        char *tok[64]; int nt = 0;
        for (char *t = strtok(line, " "); t && nt < 64; t = strtok(0, " ")) tok[nt++] = t;
        for (int i = 0; i < nt; i++) {
            int n = 0;
            if (!strcmp(tok[i], "--install") && i + 3 < nt) n = 1;
            else if (!strcmp(tok[i], "--slave") && i + 3 < nt) n = 1;
            if (!n) continue;
            const char *lnk = tok[i + 1], *target = tok[i + 3];
            if (strchr(lnk, '$') || strchr(target, '$') || lnk[0] != '/' || target[0] != '/') continue;
            struct stat st;
            if (lstat(path(lnk), &st) == 0 || stat(path(target), &st) < 0) continue;
            mkdirs(path(lnk), 0);
            if (symlink(target, path(lnk)) < 0) warn("can't link %s: %s", lnk, strerror(errno));
            else if (nalt < 64) alt_links[nalt++] = xstrdup(lnk);
        }
    }
}

struct ar_member { const char *name; const unsigned char *data; size_t size; };

static int ar_members(const unsigned char *d, size_t n, struct ar_member *m, int max)
{
    if (n < 8 || memcmp(d, "!<arch>\n", 8)) return -1;
    int k = 0;
    for (size_t off = 8; off + 60 <= n && k < max; ) {
        static char names[8][17];
        char *nm = names[k];
        memcpy(nm, d + off, 16); nm[16] = 0;
        for (int i = 15; i >= 0 && (nm[i] == ' ' || nm[i] == '/'); i--) nm[i] = 0;
        size_t size = (size_t)strtoull((const char *)d + off + 48, 0, 10);
        if (off + 60 + size > n) return -1;
        m[k].name = nm; m[k].data = d + off + 60; m[k].size = size;
        k++;
        off += 60 + size + (size & 1);
    }
    return k;
}

/* unpack a .deb into ROOT; fills the list of files */
static int unpack(const char *debpath, char ***files, int *nfiles, char **postinst)
{
    char *d; size_t n;
    if (read_file(debpath, &d, &n) < 0) { warn("can't read %s", debpath); return -1; }
    struct ar_member m[8];
    int k = ar_members((unsigned char *)d, n, m, 8);
    if (k < 3) { warn("%s isn't a .deb", debpath); free(d); return -1; }
    int ok = -1;
    *postinst = 0;
    for (int i = 0; i < k; i++) {
        int is_ctl = !strncmp(m[i].name, "control.tar", 11), is_data = !strncmp(m[i].name, "data.tar", 8);
        if (!is_ctl && !is_data) continue;
        struct tarx *t = calloc(1, sizeof *t);
        if (!t) die("out of memory");
        t->fd = -1;
        t->capture = is_ctl;
        struct dec dc;
        dec_init(&dc, comp_of(m[i].name), tar_feed, t);
        int r = 0;
        for (size_t off = 0; off < m[i].size && r == 0; off += 1 << 20) {
            size_t c = m[i].size - off < (1u << 20) ? m[i].size - off : (1u << 20);
            r = dec_feed(&dc, m[i].data + off, c);
        }
        if (dec_finish(&dc) < 0) r = -1;
        if (t->fd >= 0) close(t->fd);
        if (is_ctl) *postinst = t->postinst;
        else { *files = t->files; *nfiles = t->nfiles; ok = r; }
        free(t->meta);
        free(t);
        if (r < 0) { warn("%s: %s is damaged", debpath, m[i].name); break; }
    }
    free(d);
    return ok;
}

/* ── downloads ─────────────────────────────────────────────────────────── */
struct dl { int fd; br_sha256_context h; uint64_t n; };

static int dl_sink(void *arg, const void *data, size_t n)
{
    struct dl *d = arg;
    br_sha256_update(&d->h, data, n);
    d->n += n;
    const char *p = data;
    while (n) { ssize_t w = write(d->fd, p, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return -1; p += w; n -= (size_t)w; }
    return 0;
}

static void hexs(const unsigned char *b, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

static int sha256_file(const char *p, char out[65])
{
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    br_sha256_context h;
    br_sha256_init(&h);
    static unsigned char b[65536];
    for (;;) { ssize_t r = read(fd, b, sizeof b); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; br_sha256_update(&h, b, (size_t)r); }
    close(fd);
    unsigned char dg[32];
    br_sha256_out(&h, dg);
    hexs(dg, 32, out);
    return 0;
}

static int fetch_deb(struct pkg *p, char *out, size_t on)
{
    const char *base = strrchr(p->filename, '/');
    base = base ? base + 1 : p->filename;
    snprintf(out, on, "%s", path("/var/cache/pkg/"));
    mkdirs(out, 1);
    snprintf(out, on, "%s%s", path("/var/cache/pkg/"), base);
    char have[65];
    if (sha256_file(out, have) == 0 && !strcmp(have, p->sha256)) return 0;   /* already here */
    if (p->src < 0 || p->src >= nmirrors) { warn("%s: unknown mirror", p->name); return -1; }
    char url[2048], err[512], tmp[1100];
    snprintf(url, sizeof url, "%s/%s", mirrors[p->src], p->filename);
    snprintf(tmp, sizeof tmp, "%s.part", out);
    struct dl d = { 0 };
    d.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (d.fd < 0) { warn("can't write %s: %s", tmp, strerror(errno)); return -1; }
    br_sha256_init(&d.h);
    char label[128];
    snprintf(label, sizeof label, "%s %s", p->name, p->version);
    int r = http_get(url, dl_sink, &d, label, err, sizeof err);
    close(d.fd);
    if (r < 0) { warn("%s: %s", p->name, err); unlink(tmp); return -1; }
    unsigned char dg[32];
    br_sha256_out(&d.h, dg);
    hexs(dg, 32, have);
    if (strcmp(have, p->sha256)) { warn("%s: the download doesn't match its checksum: not installing it", p->name); unlink(tmp); return -1; }
    return rename(tmp, out);
}

/* ── install / remove ──────────────────────────────────────────────────── */
static int confirm(const char *q)
{
    if (assume_yes) { printf("%s\n", q); return 1; }
    printf("%s [Y/n] ", q);
    fflush(stdout);
    char a[16];
    if (!fgets(a, sizeof a, stdin)) return 0;
    return a[0] == '\n' || a[0] == 'y' || a[0] == 'Y';
}

static void save_list(const char *name, char **files, int n)
{
    size_t cap = 4096, len = 0;
    char *b = xmalloc(cap);
    for (int i = 0; i < n; i++) {
        size_t need = strlen(files[i]) + 2;
        if (len + need >= cap) { cap = cap * 2 + need; b = xrealloc(b, cap); }
        len += (size_t)snprintf(b + len, cap - len, "%s\n", files[i]);
    }
    char p[512];
    snprintf(p, sizeof p, "/var/lib/pkg/files/%s.list", name);
    write_file(path(p), b, len);
    free(b);
}

static int schemas_changed;

static int install_one(const char *deb, const char *name, const char *version)
{
    char **files = 0, *postinst = 0;
    int nfiles = 0;
    printf("  unpacking %s %s\n", name, version);
    if (unpack(deb, &files, &nfiles, &postinst) < 0) return -1;
    for (int i = 0; i < nfiles; i++) if (strstr(files[i], "/glib-2.0/schemas/")) schemas_changed = 1;
    alternatives(postinst);
    files = xrealloc(files, (size_t)(nfiles + nalt + 1) * sizeof *files);
    for (int i = 0; i < nalt; i++) files[nfiles++] = alt_links[i];
    save_list(name, files, nfiles);
    struct inst *in = inst_find(name);
    if (!in) { in = xmalloc(sizeof *in); in->name = xstrdup(name); in->base = 0; in->next = installed; installed = in; }
    in->version = xstrdup(version);
    in->base = 0;
    inst_save();
    for (int i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    free(postinst);
    return 0;
}

static void triggers(void)
{
    if (!schemas_changed || ROOT[0]) return;
    if (access("/usr/bin/glib-compile-schemas", X_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) { execl("/usr/bin/glib-compile-schemas", "glib-compile-schemas", "/usr/share/glib-2.0/schemas", (char *)0); _exit(127); }
        if (pid > 0) waitpid(pid, 0, 0);
    }
}

struct mem { char *b; size_t n, cap; };
static int mem_sink(void *arg, const void *data, size_t len)
{
    struct mem *mm = arg;
    if (mm->n + len + 1 > mm->cap) { mm->cap = (mm->n + len + 1) * 2; mm->b = xrealloc(mm->b, mm->cap); }
    memcpy(mm->b + mm->n, data, len); mm->n += len; mm->b[mm->n] = 0;
    return 0;
}

static int deb_field(const char *debpath, const char *field, char *out, size_t on)   /* from control.tar's control */
{
    char *d; size_t n;
    out[0] = 0;
    if (read_file(debpath, &d, &n) < 0) return -1;
    struct ar_member m[8];
    int k = ar_members((unsigned char *)d, n, m, 8);
    int found = -1;
    for (int i = 0; i < k; i++) {
        if (strncmp(m[i].name, "control.tar", 11)) continue;
        /* decompress the whole control.tar into memory and look for ./control */
        struct mem acc = { 0 };
        struct dec dc;
        dec_init(&dc, comp_of(m[i].name), mem_sink, &acc);
        dec_feed(&dc, m[i].data, m[i].size);
        dec_finish(&dc);
        for (size_t off = 0; acc.b && off + 512 <= acc.n; ) {
            const char *h = acc.b + off;
            if (!h[0]) break;
            uint64_t size = octal((const unsigned char *)h + 124, 12);
            const char *nm = h;
            while (nm[0] == '.' && nm[1] == '/') nm += 2;
            if (!strcmp(nm, "control")) {
                char *ctl = strndup(acc.b + off + 512, (size_t)size);
                char key[64];
                snprintf(key, sizeof key, "\n%s:", field);
                char *withnl = xmalloc(strlen(ctl) + 2);
                sprintf(withnl, "\n%s", ctl);
                char *p = strstr(withnl, key);
                if (p) {
                    p += strlen(key);
                    while (*p == ' ') p++;
                    size_t l = strcspn(p, "\n");
                    snprintf(out, on, "%.*s", (int)l, p);
                    found = 0;
                }
                free(withnl); free(ctl);
                break;
            }
            off += 512 + ((size + 511) & ~511ULL);
        }
        free(acc.b);
    }
    free(d);
    return found;
}

static int cmd_install(int argc, char **argv)
{
    inst_load();
    index_load(0);
    struct plan pl = { 0 };
    struct { char *path, *name, *version; } local[32];
    int nlocal = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        size_t al = strlen(a);
        if (al > 4 && !strcmp(a + al - 4, ".deb")) {                /* a local file */
            char name[256], ver[256], deps[8192], pre[4096];
            if (deb_field(a, "Package", name, sizeof name) < 0) die("%s: not a .deb I can read", a);
            deb_field(a, "Version", ver, sizeof ver);
            deb_field(a, "Depends", deps, sizeof deps);
            deb_field(a, "Pre-Depends", pre, sizeof pre);
            if (nlocal < 32) { local[nlocal].path = xstrdup(a); local[nlocal].name = xstrdup(name); local[nlocal].version = xstrdup(ver); nlocal++; }
            if (plist || index_load(0) == 0) { resolve_list(&pl, pre, name, 0); resolve_list(&pl, deps, name, 0); }
            continue;
        }
        index_load(1);
        struct pkg *p = find(a);
        if (!p) {
            struct pkg *v = provider(a);
            if (v) { printf("%s is provided by %s\n", a, v->name); p = v; }
        }
        if (!p) { warn("there's no package called %s (try `pkg search %s`)", a, a); continue; }
        struct inst *in = inst_find(p->name);
        if (in && !vercmp(in->version, p->version)) { printf("%s %s is already installed\n", p->name, p->version); continue; }
        plan_add(&pl, p, 0);
    }
    /* drop what's already there (the base system, or the same version) */
    int k = 0;
    for (int i = 0; i < pl.n; i++) {
        struct inst *in = inst_find(pl.p[i]->name);
        if (in && (in->base || !vercmp(in->version, pl.p[i]->version))) continue;
        if (is_assumed(pl.p[i]->name)) continue;
        int dup = 0;
        for (int j = 0; j < nlocal; j++) if (!strcmp(local[j].name, pl.p[i]->name)) dup = 1;
        if (!dup) pl.p[k++] = pl.p[i];
    }
    pl.n = k;
    if (!pl.n && !nlocal) { printf("Nothing to install.\n"); return 0; }
    uint64_t dl = 0, disk = 0;
    for (int i = 0; i < pl.n; i++) { dl += pl.p[i]->size; disk += pl.p[i]->isize; }
    printf("To install (%d):", pl.n + nlocal);
    int col = 13;
    for (int i = 0; i < pl.n + nlocal; i++) {
        const char *nm = i < pl.n ? pl.p[i]->name : local[i - pl.n].name;
        if (col + (int)strlen(nm) + 1 > 78) { printf("\n "); col = 1; }
        printf(" %s", nm);
        col += (int)strlen(nm) + 1;
    }
    char a[32], b[32], q[160];
    human((double)dl, a, sizeof a); human((double)disk, b, sizeof b);
    printf("\n");
    snprintf(q, sizeof q, "%s to download, %s on disk.%s", a, b, assume_yes ? "" : " Go ahead?");
    if (!confirm(q)) { printf("Nothing changed.\n"); return 1; }
    /* download everything first, then unpack (dependencies first: the plan is in that order reversed) */
    char (*debs)[1100] = xmalloc((size_t)(pl.n ? pl.n : 1) * sizeof *debs);
    for (int i = 0; i < pl.n; i++)
        if (fetch_deb(pl.p[i], debs[i], sizeof debs[i]) < 0) die("not installing anything, since %s couldn't be downloaded", pl.p[i]->name);
    int failed = 0;
    for (int i = pl.n - 1; i >= 0; i--) if (install_one(debs[i], pl.p[i]->name, pl.p[i]->version) < 0) failed++;
    for (int i = 0; i < nlocal; i++) if (install_one(local[i].path, local[i].name, local[i].version) < 0) failed++;
    triggers();
    sync();
    printf(failed ? "Done, but %d package(s) failed to unpack.\n" : "Done.\n", failed);
    return failed ? 1 : 0;
}

static int cmd_remove(int argc, char **argv)
{
    inst_load();
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct inst *in = inst_find(argv[i]);
        if (!in) { warn("%s isn't installed", argv[i]); rc = 1; continue; }
        if (in->base) { warn("%s is part of the base system: it stays", argv[i]); rc = 1; continue; }
        char lp[512], *d;
        snprintf(lp, sizeof lp, "/var/lib/pkg/files/%s.list", argv[i]);
        int n = 0;
        if (read_file(path(lp), &d, 0) == 0) {
            for (char *f = strtok(d, "\n"); f; f = strtok(0, "\n")) {
                struct stat st;
                if (lstat(path(f), &st) == 0 && !S_ISDIR(st.st_mode) && unlink(path(f)) == 0) n++;
            }
            free(d);
        }
        unlink(path(lp));
        struct inst **pp = &installed;
        while (*pp && *pp != in) pp = &(*pp)->next;
        if (*pp) *pp = in->next;
        inst_save();
        printf("Removed %s (%d files).\n", argv[i], n);
    }
    sync();
    return rc;
}

static int cmd_search(int argc, char **argv)
{
    index_load(1);
    inst_load();
    int shown = 0;
    for (size_t i = 0; i < npkg; i++) {
        struct pkg *p = plist[i];
        int all = 1;
        for (int k = 0; k < argc && all; k++) if (!strcasestr(p->name, argv[k]) && !strcasestr(p->desc, argv[k])) all = 0;
        if (!all) continue;
        printf("%s%s %s\n    %s\n", p->name, inst_find(p->name) ? " [installed]" : "", p->version, p->desc);
        if (++shown == 200) { printf("… (more: be more specific)\n"); break; }
    }
    if (!shown) printf("Nothing found.\n");
    return 0;
}

static int cmd_info(const char *name)
{
    index_load(1);
    inst_load();
    struct pkg *p = find(name);
    struct inst *in = inst_find(name);
    if (!p && !in) die("there's no package called %s", name);
    char a[32], b[32];
    printf("Package:   %s\n", name);
    if (in) printf("Installed: %s%s\n", in->version, in->base ? " (base system)" : "");
    if (p) {
        human((double)p->size, a, sizeof a); human((double)p->isize, b, sizeof b);
        printf("Available: %s\nSection:   %s\nDownload:  %s, %s installed\nDepends:   %s%s%s\nAbout:     %s\n", p->version, p->section, a, b,
               p->predepends, p->predepends[0] && p->depends[0] ? ", " : "", p->depends, p->desc);
    }
    return 0;
}

static int cmd_list(int all)
{
    inst_load();
    int n = 0;
    for (struct inst *i = installed; i; i = i->next) {
        if (i->base && !all) continue;
        printf("%-40s %s%s\n", i->name, i->version, i->base ? "  (base)" : "");
        n++;
    }
    if (!n) printf(all ? "Nothing installed.\n" : "Nothing installed with pkg yet (`pkg list --all` shows the base system).\n");
    return 0;
}

static int cmd_upgrade(void)
{
    inst_load();
    index_load(1);
    char *names[4096];
    int n = 0;
    for (struct inst *i = installed; i && n < 4096; i = i->next) {
        if (i->base) continue;
        struct pkg *p = find(i->name);
        if (p && vercmp(p->version, i->version) > 0) names[n++] = i->name;
    }
    if (!n) { printf("Everything installed with pkg is up to date.\n"); return 0; }
    return cmd_install(n, names);
}

static int cmd_clean(void)
{
    DIR *d = opendir(path("/var/cache/pkg"));
    int n = 0;
    if (d) {
        for (struct dirent *e; (e = readdir(d)); ) {
            if (e->d_name[0] == '.') continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path("/var/cache/pkg"), e->d_name);
            if (unlink(p) == 0) n++;
        }
        closedir(d);
    }
    printf("Removed %d downloaded file%s.\n", n, n == 1 ? "" : "s");
    return 0;
}

static void usage(void)
{
    printf("pkg " VERSION ": install software from the Ubuntu 24.04 archive\n\n"
           "  pkg update                  fetch the package lists (do this first)\n"
           "  pkg search WORD...          find packages\n"
           "  pkg info NAME               about one package\n"
           "  pkg install [-y] NAME...    install, with what it needs (or a ./file.deb)\n"
           "  pkg remove NAME...          remove what pkg installed\n"
           "  pkg upgrade [-y]            newer versions of what pkg installed\n"
           "  pkg list [--all]            what's installed\n"
           "  pkg clean                   delete downloaded .deb files\n\n"
           "Sources: /etc/pkg/sources. Maintainer scripts aren't run.\n");
}

int main(int argc, char **argv)
{
    const char *r = getenv("PKG_ROOT");
    snprintf(ROOT, sizeof ROOT, "%s", r ? r : "");
    size_t rl = strlen(ROOT);
    while (rl && ROOT[rl - 1] == '/') ROOT[--rl] = 0;
    quiet_progress = !isatty(2) && !getenv("PKG_PROGRESS");
    int a = 1;
    while (a < argc && argv[a][0] == '-') {
        if (!strcmp(argv[a], "-y") || !strcmp(argv[a], "--yes")) assume_yes = 1;
        else if (!strcmp(argv[a], "-h") || !strcmp(argv[a], "--help")) { usage(); return 0; }
        else break;
        a++;
    }
    if (a >= argc) { usage(); return 1; }
    const char *cmd = argv[a++];
    for (int i = a; i < argc; i++) if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes")) { assume_yes = 1; memmove(argv + i, argv + i + 1, (size_t)(argc - i) * sizeof *argv); argc--; i--; }
    /* changing the system is root's job (PKG_ROOT: building an image, any user) */
    if (geteuid() != 0 && !getenv("PKG_ROOT") && (!strcmp(cmd, "update") || !strcmp(cmd, "install") || !strcmp(cmd, "add") || !strcmp(cmd, "remove") ||
        !strcmp(cmd, "delete") || !strcmp(cmd, "uninstall") || !strcmp(cmd, "upgrade") || !strcmp(cmd, "clean"))) {
        fprintf(stderr, "pkg: %s changes the system, so it needs root: sudo pkg %s%s%s\n", cmd, cmd, a < argc ? " " : "", a < argc ? argv[a] : "");
        return 1;
    }
    if (!strcmp(cmd, "update")) return cmd_update();
    if (!strcmp(cmd, "search")) { if (a >= argc) die("search for what?"); return cmd_search(argc - a, argv + a); }
    if (!strcmp(cmd, "info") || !strcmp(cmd, "show")) { if (a >= argc) die("info about what?"); return cmd_info(argv[a]); }
    if (!strcmp(cmd, "install") || !strcmp(cmd, "add")) { if (a >= argc) die("install what?"); return cmd_install(argc - a, argv + a); }
    if (!strcmp(cmd, "remove") || !strcmp(cmd, "delete") || !strcmp(cmd, "uninstall")) { if (a >= argc) die("remove what?"); return cmd_remove(argc - a, argv + a); }
    if (!strcmp(cmd, "upgrade")) return cmd_upgrade();
    if (!strcmp(cmd, "list")) return cmd_list(a < argc && !strcmp(argv[a], "--all"));
    if (!strcmp(cmd, "clean")) return cmd_clean();
    if (!strcmp(cmd, "help")) { usage(); return 0; }
    usage();
    return 1;
}
