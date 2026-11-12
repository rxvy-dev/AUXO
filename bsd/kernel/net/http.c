/* HTTP/1.1 client with HTTPS through BearSSL (TLS 1.0–1.2, certificates checked against
 * Mozilla's roots in trust_anchors.h). One request at a time; redirects followed; chunked
 * bodies decoded. Used by the Chrono browser and the `get` shell command. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "net.h"
#include "bearssl.h"
#include "trust_anchors.h"
#include "cookies.h"

static volatile int busy;
static unsigned char seed[64];
static int seeded;

/* ── entropy: RDRAND when the CPU has it, mixed with TSC jitter ─────── */
static int has_rdrand(void) { uint32_t r[4]; cpuid(1, r); return (r[2] >> 30) & 1; }

static uint64_t rdrand64(void)
{
    uint64_t v = 0;
    unsigned char ok = 0;
    for (int i = 0; i < 10 && !ok; i++) __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    return v;
}

void tls_seed(void)
{
    br_sha256_context h;
    br_sha256_init(&h);
    int rd = has_rdrand();
    for (int i = 0; i < 256; i++) {
        uint64_t t = rdtsc(), x = rd ? rdrand64() : 0;
        br_sha256_update(&h, &t, sizeof t);
        br_sha256_update(&h, &x, sizeof x);
        for (volatile int k = 0; k < (int)(t & 63); k++) { }
    }
    struct rtc_time tm;
    rtc_read(&tm);
    br_sha256_update(&h, &tm, sizeof tm);
    br_sha256_out(&h, seed);
    br_sha256_init(&h);
    br_sha256_update(&h, seed, 32);
    uint64_t t = rdtsc();
    br_sha256_update(&h, &t, sizeof t);
    br_sha256_out(&h, seed + 32);
    seeded = 1;
}

/* days since 1 January 0 AD, as BearSSL wants for certificate validity */
static uint32_t days_since_0ad(const struct rtc_time *t)
{
    int y = t->year, m = t->month, d = t->day;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days1970 = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days1970 + 719528);
}

/* ── URL parsing ────────────────────────────────────────────────────── */
struct url { int tls; char host[128]; uint16_t port; char path[512]; };

static int parse_url(const char *u, struct url *o)
{
    memset(o, 0, sizeof *o);
    if (!strncmp(u, "https://", 8)) { o->tls = 1; o->port = 443; u += 8; }
    else if (!strncmp(u, "http://", 7)) { o->port = 80; u += 7; }
    else return -1;
    size_t i = 0;
    while (*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && i + 1 < sizeof o->host) o->host[i++] = *u++;
    if (!i) return -1;
    if (*u == ':') { u++; int p = 0; while (*u >= '0' && *u <= '9') p = p * 10 + (*u++ - '0'); if (p <= 0 || p > 65535) return -1; o->port = (uint16_t)p; }
    if (*u != '/') { o->path[0] = '/'; strlcpy(o->path + 1, u, sizeof o->path - 1); }
    else strlcpy(o->path, u, sizeof o->path);
    char *h = strchr(o->path, '#');
    if (h) *h = 0;
    return 0;
}

/* resolve a Location: header against the current URL */
static void resolve(const char *base, const char *loc, char *out, size_t n)
{
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) { strlcpy(out, loc, n); return; }
    struct url b;
    parse_url(base, &b);
    if (loc[0] == '/' && loc[1] == '/') { ksnprintf(out, n, "%s:%s", b.tls ? "https" : "http", loc); return; }
    char port[8] = "";
    if ((b.tls && b.port != 443) || (!b.tls && b.port != 80)) ksnprintf(port, sizeof port, ":%u", b.port);
    if (loc[0] == '/') { ksnprintf(out, n, "%s://%s%s%s", b.tls ? "https" : "http", b.host, port, loc); return; }
    char dir[512];
    strlcpy(dir, b.path, sizeof dir);
    char *q = strchr(dir, '?');
    if (q) *q = 0;
    char *slash = dir;
    for (char *p = dir; *p; p++) if (*p == '/') slash = p;
    slash[1] = 0;
    ksnprintf(out, n, "%s://%s%s%s%s", b.tls ? "https" : "http", b.host, port, dir, loc);
}

/* ── transport: plain TCP or BearSSL over TCP ───────────────────────── */
static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int s = *(int *)ctx;
    int n = tcp_recv(s, buf, len, 20000);
    return n > 0 ? n : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int s = *(int *)ctx;
    return tcp_send(s, buf, len) > 0 ? (int)len : -1;
}

static const char *tls_error(int e)
{
    switch (e) {
    case BR_ERR_X509_EXPIRED: return "the site's certificate is expired (or the clock is wrong)";
    case BR_ERR_X509_NOT_TRUSTED: return "the site's certificate isn't signed by a trusted authority";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the certificate is for a different site";
    case BR_ERR_UNSUPPORTED_VERSION: return "the site only speaks TLS 1.3, which isn't supported yet";
    case BR_ERR_BAD_VERSION: case BR_ERR_BAD_LENGTH: case BR_ERR_TOO_LARGE: return "the server didn't answer with TLS (is it plain http?)";
    case BR_ERR_BAD_CIPHER_SUITE: case BR_ERR_UNEXPECTED: return "no TLS cipher in common with the site";
    case BR_ERR_IO: return "the connection dropped";
    default: return 0;
    }
}

static int fetch_once(const struct http_req *q, const char *url, char *buf, size_t cap, size_t *got, struct http_result *r, void (*progress)(size_t))
{
    struct url u;
    if (parse_url(url, &u) < 0) { ksnprintf(r->error, sizeof r->error, "not an http:// or https:// address"); return -1; }
    uint32_t ip;
    if (net_resolve(u.host, &ip, 6000) < 0) { ksnprintf(r->error, sizeof r->error, "can't find %s (DNS)", u.host); return -1; }
    int s = tcp_connect(ip, u.port, 10000);
    if (s < 0) { ksnprintf(r->error, sizeof r->error, "can't connect to %s:%u", u.host, u.port); return -1; }

    static char req[8192], cookie[6144];
    char hostport[140];
    if ((u.tls && u.port != 443) || (!u.tls && u.port != 80)) ksnprintf(hostport, sizeof hostport, "%s:%u", u.host, u.port);
    else strlcpy(hostport, u.host, sizeof hostport);
    int post = q->body != 0;
    int rl = ksnprintf(req, sizeof req,
                       "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (AuxoBSD; x86_64) Chrono-Browser/0.2\r\n"
                       "Accept: text/html,text/plain,*/*\r\nAccept-Encoding: identity\r\nAccept-Language: en\r\n"
                       "Connection: close\r\n", post ? "POST" : "GET", u.path, hostport);
    if (cookies_header(u.host, u.path, u.tls, cookie, sizeof cookie))
        rl += ksnprintf(req + rl, sizeof req - (size_t)rl, "Cookie: %s\r\n", cookie);
    if (q->referer && q->referer[0] && (!u.tls || !strncmp(q->referer, "https:", 6)))
        rl += ksnprintf(req + rl, sizeof req - (size_t)rl, "Referer: %s\r\n", q->referer);
    if (post) {
        struct url ref;
        if (q->referer && parse_url(q->referer, &ref) == 0) {
            if ((ref.tls && ref.port != 443) || (!ref.tls && ref.port != 80)) rl += ksnprintf(req + rl, sizeof req - (size_t)rl, "Origin: %s://%s:%u\r\n", ref.tls ? "https" : "http", ref.host, ref.port);
            else rl += ksnprintf(req + rl, sizeof req - (size_t)rl, "Origin: %s://%s\r\n", ref.tls ? "https" : "http", ref.host);
        }
        rl += ksnprintf(req + rl, sizeof req - (size_t)rl, "Content-Type: %s\r\nContent-Length: %lu\r\n",
                        q->content_type ? q->content_type : "application/x-www-form-urlencoded", (unsigned long)q->body_len);
    }
    rl += ksnprintf(req + rl, sizeof req - (size_t)rl, "\r\n");
    if (rl >= (int)sizeof req) rl = (int)sizeof req - 1;
    size_t n = 0;
    int ok = 1;
    if (u.tls) {
        if (!seeded) tls_seed();
        br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
        struct rtc_time tm;
        rtc_read(&tm);
        br_x509_minimal_set_time(&xc, days_since_0ad(&tm), (uint32_t)(tm.hour * 3600 + tm.minute * 60 + tm.second));
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
        uint64_t t = rdtsc();
        br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof seed);
        br_ssl_engine_inject_entropy(&sc.eng, &t, sizeof t);
        br_ssl_client_reset(&sc, u.host, 0);
        br_sslio_init(&ioc, &sc.eng, sock_read, &s, sock_write, &s);
        if (br_sslio_write_all(&ioc, req, (size_t)rl) < 0) ok = 0;
        if (ok && post && q->body_len && br_sslio_write_all(&ioc, q->body, q->body_len) < 0) ok = 0;
        if (ok && br_sslio_flush(&ioc) < 0) ok = 0;
        while (ok && n < cap) {
            int k = br_sslio_read(&ioc, buf + n, cap - n);
            if (k <= 0) break;
            n += (size_t)k;
            if (progress) progress(n);
        }
        int err = br_ssl_engine_last_error(&sc.eng);
        if (!n && err) {
            const char *m = tls_error(err);
            if (!m && err >= BR_ERR_RECV_FATAL_ALERT) m = "the site refused our TLS (it may only support TLS 1.3)";
            if (m) ksnprintf(r->error, sizeof r->error, "secure connection failed: %s", m);
            else ksnprintf(r->error, sizeof r->error, "secure connection failed (TLS error %d)", err);
            tcp_close(s);
            return -1;
        }
    } else {
        if (tcp_send(s, req, (size_t)rl) < 0) ok = 0;
        if (ok && post && q->body_len && tcp_send(s, q->body, q->body_len) < 0) ok = 0;
        while (ok && n < cap) {
            int k = tcp_recv(s, buf + n, cap - n, 20000);
            if (k <= 0) break;
            n += (size_t)k;
            if (progress) progress(n);
        }
    }
    tcp_close(s);
    if (!n) { ksnprintf(r->error, sizeof r->error, "%s sent nothing back", u.host); return -1; }
    *got = n;
    return 0;
}

static int ci_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        char a = *s, b = *p;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

static size_t dechunk(char *b, size_t n)
{
    size_t in = 0, out = 0;
    for (;;) {
        size_t sz = 0;
        int digits = 0;
        while (in < n) {
            char c = b[in];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) break;
            sz = sz * 16 + (size_t)v; in++; digits++;
        }
        while (in < n && b[in] != '\n') in++;
        in++;
        if (!digits || !sz || in >= n) break;
        if (sz > n - in) sz = n - in;
        memmove(b + out, b + in, sz);
        out += sz; in += sz;
        while (in < n && (b[in] == '\r' || b[in] == '\n')) in++;
    }
    return out;
}

int http_get(const char *url, char *buf, size_t cap, struct http_result *r, void (*progress)(size_t))
{
    struct http_req q = { url, 0, 0, 0, 0 };
    return http_request(&q, buf, cap, r, progress);
}

int http_request(const struct http_req *req, char *buf, size_t cap, struct http_result *r, void (*progress)(size_t))
{
    struct http_req q = *req;
    const char *url = q.url;
    while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) rs_yield();
    memset(r, 0, sizeof *r);
    r->status = -1;
    strlcpy(r->url, url, sizeof r->url);
    if (!net_ready()) {
        ksnprintf(r->error, sizeof r->error, "no network: %s", net_status());
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        return -1;
    }
    for (int hop = 0; hop < 6; hop++) {
        size_t n = 0;
        if (fetch_once(&q, r->url, buf, cap - 1, &n, r, progress) < 0) break;
        buf[n] = 0;
        /* status line + headers */
        char *end = 0;
        for (size_t i = 0; i + 3 < n; i++) if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { end = buf + i; break; }
        if (strncmp(buf, "HTTP/", 5) || !end) { ksnprintf(r->error, sizeof r->error, "not an HTTP reply"); break; }
        char *sp = strchr(buf, ' ');
        r->status = sp ? katoi(sp + 1) : -1;
        int chunked = 0;
        char location[512] = "";
        struct url cu;
        parse_url(r->url, &cu);
        for (char *line = strchr(buf, '\n') + 1; line < end; ) {
            char *nl = strchr(line, '\n');
            if (!nl || nl > end + 2) nl = end;
            char save = *nl; *nl = 0;
            char *cr = strchr(line, '\r'); if (cr) *cr = 0;
            if (ci_prefix(line, "set-cookie:")) { char *v = line + 11; while (*v == ' ') v++; cookies_set(cu.host, cu.path, cu.tls, v); }
            else if (ci_prefix(line, "location:")) { char *v = line + 9; while (*v == ' ') v++; strlcpy(location, v, sizeof location); }
            else if (ci_prefix(line, "content-type:")) { char *v = line + 13; while (*v == ' ') v++; strlcpy(r->content_type, v, sizeof r->content_type); }
            else if (ci_prefix(line, "transfer-encoding:") && strchr(line, 'c')) chunked = 1;
            if (cr) *cr = '\r';
            *nl = save;
            line = nl + 1;
        }
        char *body = end + 4;
        size_t blen = n - (size_t)(body - buf);
        if (r->status >= 300 && r->status < 400 && location[0]) {
            char next[512];
            resolve(r->url, location, next, sizeof next);
            strlcpy(r->url, next, sizeof r->url);
            if (r->status != 307 && r->status != 308) { q.body = 0; q.body_len = 0; }   /* POST → GET */
            continue;
        }
        if (chunked) blen = dechunk(body, blen);
        memmove(buf, body, blen);
        buf[blen] = 0;
        r->body = buf;
        r->len = blen;
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        cookies_save();
        return 0;
    }
    if (r->status >= 300 && r->status < 400 && !r->error[0]) ksnprintf(r->error, sizeof r->error, "too many redirects");
    if (r->status > 0 && !r->error[0]) r->status = -1;
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    cookies_save();
    return -1;
}
