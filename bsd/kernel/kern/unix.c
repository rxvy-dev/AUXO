/* AF_UNIX stream sockets: connect/accept by path (or Linux's abstract names), byte streams
 * with file descriptors passed alongside (SCM_RIGHTS). The Chrono Wayland server uses the
 * same sockets from inside the kernel. */
#include "kern.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "unix.h"

#define RXCAP (256 * 1024)
static uint64_t irq_off(void);
static void irq_on(uint64_t f);

static struct usock *abstract[16];
static char abstract_name[16][108];

struct usock *unix_new(void)
{
    struct usock *s = rs_kmalloc(sizeof *s);
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    uint64_t pg = pmm_alloc_contig(RXCAP / PAGE_SIZE);
    if (!pg) { rs_kfree(s); return 0; }
    s->rx = (uint8_t *)(uintptr_t)pg;
    s->cap = RXCAP;
    s->refs = 1;
    return s;
}

static void drop_fds(struct usock *s)
{
    for (int i = 0; i < s->nfds; i++) file_put(s->fds[i].f);
    s->nfds = 0;
}

void unix_close(struct usock *s)
{
    if (!s || --s->refs > 0) return;
    uint64_t fl = irq_off();
    if (s->peer) { s->peer->peer_closed = 1; s->peer->peer = 0; }
    for (int i = 0; i < s->nback; i++) { s->backlog[i]->peer_closed = 1; unix_close(s->backlog[i]); }
    for (int i = 0; i < 16; i++) if (abstract[i] == s) abstract[i] = 0;
    if (s->node) { s->node->priv = 0; fs_put(s->node); }
    drop_fds(s);
    irq_on(fl);
    for (uint64_t o = 0; o < s->cap; o += PAGE_SIZE) pmm_free_page((uint64_t)(uintptr_t)s->rx + o);
    rs_kfree(s);
}

int unix_bind(struct usock *s, const char *path, struct node *cwd)
{
    if (path[0] == 0) {                                  /* abstract namespace: "\0name" */
        for (int i = 0; i < 16; i++)
            if (!abstract[i]) { abstract[i] = s; strlcpy(abstract_name[i], path + 1, sizeof abstract_name[i]); return 0; }
        return -98;
    }
    struct node *dir, *n;
    char name[256];
    int r = fs_lookup_parent(cwd, path, &dir, name, sizeof name);
    if (r < 0) return r;
    if (fs_child(dir, name)) return -98;                 /* EADDRINUSE */
    n = fs_create(dir, name, N_SOCK, 0777);
    if (!n) return -12;
    n->priv = s;
    n->refs++;
    s->node = n;
    return 0;
}

int unix_listen(struct usock *s) { s->listening = 1; return 0; }

static struct usock *lookup(const char *path, struct node *cwd)
{
    if (path[0] == 0) {
        for (int i = 0; i < 16; i++) if (abstract[i] && !strcmp(abstract_name[i], path + 1)) return abstract[i];
        return 0;
    }
    struct node *n;
    if (fs_lookup(cwd, path, 1, &n) < 0 || n->type != N_SOCK) return 0;
    return n->priv;
}

int unix_connect(struct usock *s, const char *path, struct node *cwd)
{
    struct usock *l = lookup(path, cwd);
    if (!l) return -111;                                 /* ECONNREFUSED */
    if (!l->listening || l->nback >= 16) return -111;
    struct usock *srv = unix_new();
    if (!srv) return -12;
    srv->peer = s; s->peer = srv;
    l->backlog[l->nback++] = srv;
    s->connected = srv->connected = 1;
    return 0;
}

struct usock *unix_accept(struct usock *l)
{
    if (!l->nback) return 0;
    struct usock *s = l->backlog[0];
    memmove(l->backlog, l->backlog + 1, sizeof l->backlog[0] * (size_t)(l->nback - 1));
    l->nback--;
    return s;
}

int unix_pair(struct usock **a, struct usock **b)
{
    *a = unix_new(); *b = unix_new();
    if (!*a || !*b) return -12;
    (*a)->peer = *b; (*b)->peer = *a;
    (*a)->connected = (*b)->connected = 1;
    return 0;
}

static uint64_t irq_off(void) { uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static void irq_on(uint64_t f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

static long send_locked(struct usock *s, const void *data, size_t len, struct file **files, int nfiles);
static long recv_locked(struct usock *s, void *buf, size_t len, struct file **files, int *nfiles, int maxfiles);

long unix_send(struct usock *s, const void *data, size_t len, struct file **files, int nfiles)
{
    uint64_t f = irq_off();
    long r = send_locked(s, data, len, files, nfiles);
    irq_on(f);
    return r;
}

long unix_recv(struct usock *s, void *buf, size_t len, struct file **files, int *nfiles, int maxfiles)
{
    uint64_t f = irq_off();
    long r = recv_locked(s, buf, len, files, nfiles, maxfiles);
    irq_on(f);
    return r;
}

static long send_locked(struct usock *s, const void *data, size_t len, struct file **files, int nfiles)
{
    struct usock *p = s->peer;
    if (!p || s->peer_closed) return -32;               /* EPIPE */
    size_t room = p->cap - p->len;
    if (!room) return -11;
    if (len > room) len = room;
    for (int i = 0; i < nfiles && p->nfds < UNIX_MAXFDS; i++) {
        files[i]->refs++;
        p->fds[p->nfds].f = files[i];
        p->fds[p->nfds].at = p->total_in;
        p->nfds++;
    }
    for (size_t i = 0; i < len; i++) p->rx[(p->head + p->len + i) % p->cap] = ((const uint8_t *)data)[i];
    p->len += (uint32_t)len;
    p->total_in += len;
    return (long)len;
}

static long recv_locked(struct usock *s, void *buf, size_t len, struct file **files, int *nfiles, int maxfiles)
{
    if (!s->len) return s->peer_closed || !s->peer ? 0 : -11;
    /* don't read past the start of a later message's file descriptors in one go */
    size_t n = len < s->len ? len : s->len;
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = s->rx[(s->head + i) % s->cap];
    uint64_t start = s->total_out;
    s->head = (s->head + (uint32_t)n) % s->cap;
    s->len -= (uint32_t)n;
    s->total_out += n;
    int got = 0;
    while (s->nfds && s->fds[0].at < start + n) {
        if (files && got < maxfiles) files[got++] = s->fds[0].f;
        else file_put(s->fds[0].f);
        memmove(s->fds, s->fds + 1, sizeof s->fds[0] * (size_t)(s->nfds - 1));
        s->nfds--;
    }
    if (nfiles) *nfiles = got;
    return (long)n;
}

int unix_readable(struct usock *s) { return s->listening ? s->nback > 0 : (s->len > 0 || s->peer_closed || !s->peer); }
int unix_writable(struct usock *s) { return s->peer && s->peer->len < s->peer->cap; }
