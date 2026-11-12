/* The browser's cookie jar: what keeps you signed in to sites. Every request http.c makes gets
 * the matching cookies, every reply's Set-Cookie (redirects included, which is where sign-in
 * flows set them) is stored. Domain, Path, Expires, Max-Age and Secure are honoured. The jar
 * lives in /home/root/.config/browser/cookies, so an installed system keeps it on its disk;
 * session cookies are kept too, like a browser that reopens where you left off. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "vfs.h"
#include "cookies.h"

#define MAXC 300
#define JAR "/home/root/.config/browser/cookies"

struct cookie {
    char domain[96], path[96], name[64];
    char *value;
    uint64_t expires;                   /* unix time; 0 = session */
    uint8_t hostonly, secure, used;
};
static struct cookie jar[MAXC];
static int loaded, dirty;
static volatile int lock;

static void jar_lock(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) rs_yield(); }
static void jar_unlock(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ci_eqn(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) if (lc(a[i]) != lc(b[i])) return 0; return 1; }

uint64_t unix_now(void)
{
    struct rtc_time t;
    rtc_read(&t);
    int y = t.year, m = t.month;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1);
    long days = (long)era * 146097 + (long)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    return (uint64_t)days * 86400 + (uint64_t)(t.hour * 3600 + t.minute * 60 + t.second);
}

/* "Wed, 21 Oct 2026 07:28:00 GMT" (also with dashes, or a 2-digit year) */
static uint64_t parse_date(const char *s)
{
    static const char mon[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int day = 0, month = 0, year = 0, hh = 0, mm = 0, ss = 0;
    while (*s) {
        while (*s && !((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) s++;
        if (!*s) break;
        if (*s >= '0' && *s <= '9') {
            int v = 0, digits = 0;
            const char *st = s;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); digits++; }
            if (*s == ':' && !hh && !mm) {               /* hh:mm:ss */
                hh = v; s++;
                mm = katoi(s); while (*s >= '0' && *s <= '9') s++;
                if (*s == ':') { s++; ss = katoi(s); while (*s >= '0' && *s <= '9') s++; }
            } else if (digits >= 3 || (day && month)) year = v;
            else if (!day) day = v;
            else year = v;
            (void)st;
        } else {
            char w[4] = { (char)lc(s[0]), (char)lc(s[1]), (char)lc(s[2]), 0 };
            for (int i = 0; i < 12; i++) if (!memcmp(mon + 3 * i, w, 3)) month = i + 1;
            while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
        }
    }
    if (year < 70) year += 2000; else if (year < 100) year += 1900;
    if (!day || !month || year < 1970) return 0;
    int y = year - (month <= 2);
    int era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    long days = (long)era * 146097 + (long)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    return (uint64_t)days * 86400 + (uint64_t)(hh * 3600 + mm * 60 + ss);
}

static void forget(struct cookie *c) { if (c->value) rs_kfree(c->value); memset(c, 0, sizeof *c); }

static void put(const char *domain, int hostonly, const char *path, const char *name, const char *value,
                uint64_t expires, int secure)
{
    struct cookie *slot = 0, *oldest = 0;
    for (int i = 0; i < MAXC; i++) {
        struct cookie *c = &jar[i];
        if (!c->used) { if (!slot) slot = c; continue; }
        if (!strcmp(c->domain, domain) && !strcmp(c->path, path) && !strcmp(c->name, name)) { forget(c); slot = c; break; }
        if (!oldest || (c->expires && c->expires < oldest->expires)) oldest = c;
    }
    uint64_t now = unix_now();
    if (expires && expires <= now) { dirty = 1; return; }        /* a delete */
    if (!slot) { slot = oldest ? oldest : &jar[0]; forget(slot); }
    size_t vl = strlen(value);
    slot->value = rs_kmalloc(vl + 1);
    if (!slot->value) return;
    memcpy(slot->value, value, vl + 1);
    strlcpy(slot->domain, domain, sizeof slot->domain);
    strlcpy(slot->path, path, sizeof slot->path);
    strlcpy(slot->name, name, sizeof slot->name);
    slot->expires = expires; slot->hostonly = (uint8_t)hostonly; slot->secure = (uint8_t)secure; slot->used = 1;
    dirty = 1;
}

/* ── the file: domain \t hostonly \t path \t secure \t expires \t name \t value ── */
static void load(void)
{
    if (loaded) return;
    loaded = 1;
    const char *d; size_t n;
    if (vfs_read(JAR, &d, &n) < 0) return;
    char *copy = rs_kmalloc(n + 1);
    if (!copy) return;
    memcpy(copy, d, n); copy[n] = 0;
    uint64_t now = unix_now();
    for (char *line = copy; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *f[7]; int k = 0;
        f[k++] = line;
        for (char *p = line; *p && k < 7; p++) if (*p == '\t') { *p = 0; f[k++] = p + 1; }
        if (k == 7) {
            uint64_t ex = 0; for (const char *p = f[4]; *p >= '0' && *p <= '9'; p++) ex = ex * 10 + (uint64_t)(*p - '0');
            if (!ex || ex > now) put(f[0], f[1][0] == '1', f[2], f[5], f[6], ex, f[3][0] == '1');
        }
        line = nl ? nl + 1 : 0;
    }
    rs_kfree(copy);
    dirty = 0;
}

void cookies_save(void)
{
    jar_lock();
    if (!dirty) { jar_unlock(); return; }
    size_t cap = 4096;
    for (int i = 0; i < MAXC; i++) if (jar[i].used) cap += strlen(jar[i].value) + 300;
    char *b = rs_kmalloc(cap);
    if (!b) { jar_unlock(); return; }
    size_t o = 0;
    for (int i = 0; i < MAXC; i++) {
        struct cookie *c = &jar[i];
        if (!c->used) continue;
        o += (size_t)ksnprintf(b + o, cap - o, "%s\t%d\t%s\t%d\t%lu\t%s\t%s\n", c->domain, c->hostonly, c->path, c->secure,
                               (unsigned long)c->expires, c->name, c->value);
    }
    vfs_write(JAR, b, o);
    rs_kfree(b);
    dirty = 0;
    jar_unlock();
}

static int domain_match(const char *host, const struct cookie *c)
{
    if (c->hostonly) return !strcmp(host, c->domain);
    size_t hl = strlen(host), dl = strlen(c->domain);
    if (hl == dl) return ci_eqn(host, c->domain, hl);
    return hl > dl && host[hl - dl - 1] == '.' && ci_eqn(host + hl - dl, c->domain, dl);
}

static int path_match(const char *path, const char *cp)
{
    size_t n = strlen(cp);
    if (strncmp(path, cp, n)) return 0;
    return path[n] == 0 || path[n] == '/' || path[n] == '?' || cp[n - 1] == '/';
}

size_t cookies_header(const char *host, const char *path, int secure, char *out, size_t cap)
{
    jar_lock();
    load();
    uint64_t now = unix_now();
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < MAXC; i++) {
        struct cookie *c = &jar[i];
        if (!c->used) continue;
        if (c->expires && c->expires <= now) { forget(c); dirty = 1; continue; }
        if ((c->secure && !secure) || !domain_match(host, c) || !path_match(path, c->path)) continue;
        size_t need = strlen(c->name) + strlen(c->value) + 3;
        if (o + need >= cap) break;
        o += (size_t)ksnprintf(out + o, cap - o, "%s%s=%s", o ? "; " : "", c->name, c->value);
    }
    jar_unlock();
    return o;
}

/* one Set-Cookie header value, from a reply to host/path */
void cookies_set(const char *host, const char *reqpath, int secure, const char *v)
{
    (void)secure;
    jar_lock();
    load();
    char name[64] = "", value[4096] = "", domain[96] = "", path[96] = "";
    uint64_t expires = 0;
    int is_secure = 0, have_maxage = 0;
    const char *p = v;
    /* name=value */
    const char *eq = strchr(p, '='), *semi = strchr(p, ';');
    if (!eq || (semi && semi < eq)) { jar_unlock(); return; }
    size_t nl = (size_t)(eq - p);
    while (nl && p[nl - 1] == ' ') nl--;
    if (!nl || nl >= sizeof name) { jar_unlock(); return; }
    memcpy(name, p, nl); name[nl] = 0;
    const char *ve = semi ? semi : eq + strlen(eq);
    size_t vl = (size_t)(ve - eq - 1);
    if (vl >= sizeof value) { jar_unlock(); return; }
    memcpy(value, eq + 1, vl); value[vl] = 0;
    /* attributes */
    for (p = semi; p && *p; ) {
        p++;
        while (*p == ' ') p++;
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char a[256];
        if (len >= sizeof a) len = sizeof a - 1;
        memcpy(a, p, len); a[len] = 0;
        char *av = strchr(a, '=');
        if (av) *av++ = 0;
        if (ci_eqn(a, "expires", 8) && av && !have_maxage) expires = parse_date(av);
        else if (ci_eqn(a, "max-age", 8) && av) {
            long s = katoi(av);
            expires = s <= 0 ? 1 : unix_now() + (uint64_t)s;
            have_maxage = 1;
        }
        else if (ci_eqn(a, "domain", 7) && av) { while (*av == '.') av++; strlcpy(domain, av, sizeof domain); }
        else if (ci_eqn(a, "path", 5) && av && av[0] == '/') strlcpy(path, av, sizeof path);
        else if (ci_eqn(a, "secure", 7)) is_secure = 1;
        p = e;
    }
    for (char *q = domain; *q; q++) *q = (char)lc(*q);
    int hostonly = 1;
    if (domain[0]) {                                     /* must cover the host that set it */
        size_t hl = strlen(host), dl = strlen(domain);
        if (!(hl == dl && ci_eqn(host, domain, hl)) && !(hl > dl && host[hl - dl - 1] == '.' && ci_eqn(host + hl - dl, domain, dl))) { jar_unlock(); return; }
        if (!strchr(domain, '.')) { jar_unlock(); return; }   /* no cookies for a whole TLD */
        hostonly = 0;
    } else {
        strlcpy(domain, host, sizeof domain);
        for (char *q = domain; *q; q++) *q = (char)lc(*q);
    }
    if (!path[0]) {                                      /* default: the request's directory */
        strlcpy(path, reqpath, sizeof path);
        char *qm = strchr(path, '?'); if (qm) *qm = 0;
        char *last = strrchr(path, '/');
        if (!last || last == path) strlcpy(path, "/", sizeof path); else *last = 0;
    }
    put(domain, hostonly, path, name, value, expires, is_secure);
    jar_unlock();
}

int cookies_count(void)
{
    jar_lock(); load();
    int n = 0;
    for (int i = 0; i < MAXC; i++) n += jar[i].used;
    jar_unlock();
    return n;
}

void cookies_clear(void)
{
    jar_lock(); load();
    for (int i = 0; i < MAXC; i++) if (jar[i].used) forget(&jar[i]);
    dirty = 1;
    jar_unlock();
    cookies_save();
}
