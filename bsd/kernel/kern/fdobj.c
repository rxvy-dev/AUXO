/* File-like kernel objects programs wait on with poll/epoll, and POSIX timers:
 *   inotify  (kind 11): file and directory change events; fs.c reports every change here
 *   timerfd  (kind 12): a timer that becomes readable when it fires
 *   pidfd    (kind 13): a process; readable once it has exited
 *   timer_create …     : per-process timers that send a signal
 * GLib uses all of these (file monitors, timeouts, child watches); so do media players,
 * editors and file managers. */
#include "kern.h"
#include "arch.h"
#include "rs.h"
#include "fs.h"
#include "uproc.h"
#include "ns.h"
#include "netlink.h"
#include "fdobj.h"

enum { EBADF = 9, EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EEXIST = 17, ENOTDIR = 20, EINVAL = 22, ENOSPC = 28, ESRCH = 3, EINTR = 4 };

/* ── inotify ────────────────────────────────────────────────────────── */
#define IN_ISDIR      0x40000000u
#define IN_IGNORED    0x00008000u
#define IN_Q_OVERFLOW 0x00004000u
#define IN_ONESHOT    0x80000000u
#define IN_ONLYDIR    0x01000000u
#define IN_DONT_FOLLOW 0x02000000u
#define IN_MASK_ADD   0x20000000u
#define IN_MASK_CREATE 0x10000000u
#define IN_ALL_EVENTS 0x00000FFFu
#define INO_WATCHES 512
#define INO_QUEUE 16384

struct watch { int wd; uint32_t mask; struct node *n; };
struct ino {
    struct ino *next;
    int nextwd, nw;
    struct watch w[INO_WATCHES];
    uint32_t qlen, lastlen;
    uint8_t q[INO_QUEUE];
};
static struct ino *inos;
static uint32_t cookies;

static void queue(struct ino *in, int wd, uint32_t mask, uint32_t cookie, const char *name)
{
    size_t nl = name ? strlen(name) + 1 : 0;
    uint32_t len = nl ? (uint32_t)((nl + 15) & ~15UL) : 0;           /* padded, as Linux does */
    uint32_t total = 16 + len;
    /* the same event twice in a row (a file written in pieces) is one event */
    if (in->lastlen && in->lastlen == total && in->qlen >= total) {
        uint8_t *l = in->q + in->qlen - total;
        int lwd; uint32_t lmask, lcookie;
        memcpy(&lwd, l, 4); memcpy(&lmask, l + 4, 4); memcpy(&lcookie, l + 8, 4);
        if (lwd == wd && lmask == mask && lcookie == cookie && (!nl || !strcmp((char *)l + 16, name))) return;
    }
    if (in->qlen + total > INO_QUEUE - 32) {                       /* full: one overflow event, then nothing */
        if (in->qlen + 16 <= INO_QUEUE) {
            uint8_t *e = in->q + in->qlen;
            int m1 = -1; uint32_t ov = IN_Q_OVERFLOW, z = 0;
            memcpy(e, &m1, 4); memcpy(e + 4, &ov, 4); memcpy(e + 8, &z, 4); memcpy(e + 12, &z, 4);
            in->qlen += 16; in->lastlen = 0;
        }
        return;
    }
    uint8_t *e = in->q + in->qlen;
    memset(e, 0, total);
    memcpy(e, &wd, 4); memcpy(e + 4, &mask, 4); memcpy(e + 8, &cookie, 4); memcpy(e + 12, &len, 4);
    if (nl) memcpy(e + 16, name, nl);
    in->qlen += total;
    in->lastlen = total;
}

static void drop_watch(struct ino *in, int i, int ignored)
{
    if (ignored) queue(in, in->w[i].wd, IN_IGNORED, 0, 0);
    struct node *n = in->w[i].n;
    in->w[i] = in->w[--in->nw];
    if (n && --n->refs <= 0 && n->unlinked && n->nlink == 0) {     /* we were the last to hold it (fs_lock held) */
        n->refs = 1;
        fs_unlock();                                                /* (recursive lock: still ours) */
        fs_put(n);
        fs_lock();
    }
}

/* every change to the tree, from fs.c (fs_lock held): dir/name changed, n is the node itself */
void inotify_event(struct node *dir, struct node *n, const char *name, uint32_t mask, uint32_t cookie)
{
    if (!inos) return;
    uint32_t isdir = n && n->type == N_DIR ? IN_ISDIR : 0;
    for (struct ino *in = inos; in; in = in->next)
        for (int i = 0; i < in->nw; i++) {
            struct watch *w = &in->w[i];
            if (dir && w->n == dir && (w->mask & mask & IN_ALL_EVENTS))
                queue(in, w->wd, (mask & IN_ALL_EVENTS) | isdir, cookie, name);
            else if (n && w->n == n && (w->mask & mask & IN_ALL_EVENTS) && mask != 0x100 && mask != 0x200 && mask != 0x40 && mask != 0x80)
                queue(in, w->wd, (mask & IN_ALL_EVENTS) | isdir, cookie, 0);   /* the watched thing itself (not child events) */
            else continue;
            if (w->mask & IN_ONESHOT) { drop_watch(in, i, 1); i--; }
        }
    /* deleted: its watches end */
    if (n && (mask & 0x400))
        for (struct ino *in = inos; in; in = in->next)
            for (int i = 0; i < in->nw; i++) if (in->w[i].n == n) { drop_watch(in, i, 1); i--; }
}

uint32_t inotify_cookie(void) { return ++cookies ? cookies : ++cookies; }

long sys_inotify_init(struct task *t, long flags)
{
    if (flags & ~(04000L | 02000000L)) return -EINVAL;
    struct ino *in = rs_kmalloc(sizeof *in);
    if (!in) return -ENOMEM;
    memset(in, 0, sizeof *in);
    in->nextwd = 1;
    struct file *f = file_new(11, 0, 2 | (int)(flags & 04000));
    if (!f) { rs_kfree(in); return -ENOMEM; }
    f->priv = in;
    strlcpy(f->path, "anon_inode:inotify", sizeof f->path);
    fs_lock();
    in->next = inos; inos = in;
    fs_unlock();
    return nl_install(t, f, (flags & 02000000) != 0);
}

static struct ino *getino(struct task *t, long fd)
{
    struct file *f = t->files && fd >= 0 && fd < MAXFD ? t->files->fd[fd] : 0;
    return f && f->kind == 11 ? f->priv : 0;
}

long sys_inotify_add(struct task *t, long fd, uint64_t upath, uint32_t mask)
{
    struct ino *in = getino(t, fd);
    if (!in) return -EBADF;
    if (!(mask & (IN_ALL_EVENTS))) return -EINVAL;
    char path[1024];
    long r = copyin_str(t->mm, path, upath, sizeof path);
    if (r < 0) return r;
    struct path p;
    if ((r = vfs_walk(t, t->ns.cwd, path, !(mask & IN_DONT_FOLLOW), &p)) < 0) return r;
    if ((mask & IN_ONLYDIR) && p.n->type != N_DIR) return -ENOTDIR;
    fs_lock();
    for (int i = 0; i < in->nw; i++)
        if (in->w[i].n == p.n) {
            if (mask & IN_MASK_CREATE) { fs_unlock(); return -EEXIST; }
            in->w[i].mask = (mask & IN_MASK_ADD) ? in->w[i].mask | mask : mask;
            int wd = in->w[i].wd;
            fs_unlock();
            return wd;
        }
    if (in->nw >= INO_WATCHES) { fs_unlock(); return -ENOSPC; }
    struct watch *w = &in->w[in->nw++];
    w->wd = in->nextwd++; w->mask = mask; w->n = p.n;
    p.n->refs++;
    int wd = w->wd;
    fs_unlock();
    return wd;
}

long sys_inotify_rm(struct task *t, long fd, long wd)
{
    struct ino *in = getino(t, fd);
    if (!in) return -EBADF;
    fs_lock();
    for (int i = 0; i < in->nw; i++)
        if (in->w[i].wd == wd) { drop_watch(in, i, 1); fs_unlock(); return 0; }
    fs_unlock();
    return -EINVAL;
}

/* ── timerfd ────────────────────────────────────────────────────────── */
struct tfd { int clock; uint64_t next, interval, count; };   /* nanoseconds on the monotonic clock */

static uint64_t epoch_ns(void)        /* CLOCK_REALTIME minus CLOCK_MONOTONIC */
{
    int64_t s, ns;
    clock_now(0, &s, &ns);
    uint64_t real = (uint64_t)s * 1000000000ULL + (uint64_t)ns, mono = mono_ns();
    return real > mono ? real - mono : 0;
}

static void tfd_update(struct tfd *x)
{
    uint64_t now = mono_ns();
    if (!x->next || now < x->next) return;
    uint64_t n = 1;
    if (x->interval) { n += (now - x->next) / x->interval; x->next += n * x->interval; }
    else x->next = 0;
    x->count += n;
}

long sys_timerfd_create(struct task *t, long clock, long flags)
{
    if (clock != 0 && clock != 1 && clock != 7 && clock != 8 && clock != 9) return -EINVAL;
    if (flags & ~(04000L | 02000000L)) return -EINVAL;
    struct tfd *x = rs_kmalloc(sizeof *x);
    if (!x) return -ENOMEM;
    memset(x, 0, sizeof *x);
    x->clock = (int)clock;
    struct file *f = file_new(12, 0, 2 | (int)(flags & 04000));
    if (!f) { rs_kfree(x); return -ENOMEM; }
    f->priv = x;
    strlcpy(f->path, "anon_inode:[timerfd]", sizeof f->path);
    return nl_install(t, f, (flags & 02000000) != 0);
}

static struct tfd *gettfd(struct task *t, long fd)
{
    struct file *f = t->files && fd >= 0 && fd < MAXFD ? t->files->fd[fd] : 0;
    return f && f->kind == 12 ? f->priv : 0;
}

static void ts_out(uint64_t ns, int64_t o[2]) { o[0] = (int64_t)(ns / 1000000000ULL); o[1] = (int64_t)(ns % 1000000000ULL); }
static uint64_t ts_in(const int64_t v[2]) { return v[0] < 0 || v[1] < 0 ? 0 : (uint64_t)v[0] * 1000000000ULL + (uint64_t)v[1]; }

static void itimer_out(struct task *t, uint64_t ua, uint64_t next, uint64_t interval)
{
    int64_t o[4];
    uint64_t now = mono_ns();
    ts_out(interval, o);
    ts_out(next > now ? next - now : next ? 1 : 0, o + 2);
    copyout(t->mm, ua, o, 32);
}

long sys_timerfd_settime(struct task *t, long fd, long flags, uint64_t unew, uint64_t uold)
{
    struct tfd *x = gettfd(t, fd);
    if (!x) return -EBADF;
    int64_t v[4];
    if (copyin(t->mm, v, unew, 32) < 0) return -EFAULT;
    if (v[1] >= 1000000000 || v[3] >= 1000000000 || v[1] < 0 || v[3] < 0) return -EINVAL;
    tfd_update(x);
    if (uold) itimer_out(t, uold, x->next, x->interval);
    uint64_t val = ts_in(v + 2);
    x->interval = ts_in(v);
    x->count = 0;
    if (!val) { x->next = 0; return 0; }
    if (flags & 1) {                                          /* TFD_TIMER_ABSTIME */
        if (x->clock == 0 || x->clock == 8) { uint64_t e = epoch_ns(); val = val > e ? val - e : 1; }
        x->next = val ? val : 1;
    } else x->next = mono_ns() + val;
    return 0;
}

long sys_timerfd_gettime(struct task *t, long fd, uint64_t ucur)
{
    struct tfd *x = gettfd(t, fd);
    if (!x) return -EBADF;
    tfd_update(x);
    itimer_out(t, ucur, x->next, x->interval);
    return 0;
}

/* ── pidfd ──────────────────────────────────────────────────────────── */
long sys_pidfd_open(struct task *t, long pid, long flags)
{
    if (flags & ~04000L) return -EINVAL;
    int g = pid_global(t, (int)pid, 1);
    struct task *x = g ? task_by_pid(g) : 0;
    if (!x || x->is_thread) return -ESRCH;
    struct file *f = file_new(13, 0, 2 | (int)(flags & 04000));
    if (!f) return -ENOMEM;
    f->priv = (void *)(uintptr_t)x->tgid;
    ksnprintf(f->path, sizeof f->path, "anon_inode:[pidfd]");
    return nl_install(t, f, 1);                               /* pidfds are always close-on-exec */
}

int pidfd_pid(struct file *f) { return f && f->kind == 13 ? (int)(uintptr_t)f->priv : 0; }

static int pid_done(int g)
{
    struct task *x = task_by_pid(g);
    return !x || !x->used || x->state == 2 || x->tgid != g;
}

/* ── read and poll ──────────────────────────────────────────────────── */
long fdobj_read(struct task *t, struct file *f, void *kbuf, size_t n)
{
    if (f->kind == 11) {
        struct ino *in = f->priv;
        for (;;) {
            fs_lock();
            if (in->qlen) {
                uint32_t first; memcpy(&first, in->q + 12, 4);
                if (n < 16 + first) { fs_unlock(); return -EINVAL; }
                size_t o = 0;
                while (o < in->qlen) {                          /* whole events that fit */
                    uint32_t l; memcpy(&l, in->q + o + 12, 4);
                    if (o + 16 + l > n) break;
                    o += 16 + l;
                }
                memcpy(kbuf, in->q, o);
                memmove(in->q, in->q + o, in->qlen - o);
                in->qlen -= (uint32_t)o;
                in->lastlen = 0;
                fs_unlock();
                return (long)o;
            }
            fs_unlock();
            if (f->flags & 04000) return -EAGAIN;
            if (t && (t->pending & ~t->sigmask)) return -EINTR;
            wait_tick();
        }
    }
    if (f->kind == 12) {
        struct tfd *x = f->priv;
        if (n < 8) return -EINVAL;
        for (;;) {
            tfd_update(x);
            if (x->count) { memcpy(kbuf, &x->count, 8); x->count = 0; return 8; }
            if (f->flags & 04000) return -EAGAIN;
            if (t && (t->pending & ~t->sigmask)) return -EINTR;
            wait_tick();
        }
    }
    return -EINVAL;
}

int fdobj_ready(struct task *t, struct file *f)
{
    (void)t;
    if (f->kind == 11) return ((struct ino *)f->priv)->qlen ? 1 : 0;
    if (f->kind == 12) { struct tfd *x = f->priv; tfd_update(x); return x->count ? 1 : 0; }
    if (f->kind == 13) return pid_done(pidfd_pid(f)) ? 1 : 0;
    return 0;
}

void fdobj_close(struct file *f)
{
    if (f->kind == 11) {
        struct ino *in = f->priv;
        fs_lock();
        struct ino **pp = &inos;
        while (*pp && *pp != in) pp = &(*pp)->next;
        if (*pp) *pp = in->next;
        while (in->nw) drop_watch(in, in->nw - 1, 0);
        fs_unlock();
        rs_kfree(in);
    } else if (f->kind == 12) rs_kfree(f->priv);
}

/* ── POSIX timers ───────────────────────────────────────────────────── */
#define NPTIMER 256
static struct ptimer {
    int used, tgid, id, tid, signo, notify, overrun;
    uint64_t next, interval, value;
} pt[NPTIMER];

long sys_timer_create(struct task *t, long clock, uint64_t usev, uint64_t uid)
{
    if (clock < 0 || clock > 11) return -EINVAL;
    struct { uint64_t value; int signo, notify, tid, pad[11]; } sev = { 0, 14, 0, 0, { 0 } };    /* default: SIGALRM */
    if (usev && copyin(t->mm, &sev, usev, 64) < 0) return -EFAULT;
    if (sev.notify != 0 && sev.notify != 1 && sev.notify != 4) return -EINVAL;   /* SIGNAL, NONE, THREAD_ID */
    if (sev.notify != 1 && (sev.signo < 1 || sev.signo > 64)) return -EINVAL;
    if (sev.notify == 4 && !pid_global(t, sev.tid, 0)) return -EINVAL;
    int id = 0;
    for (int i = 0; i < NPTIMER; i++) if (pt[i].used && pt[i].tgid == t->tgid && pt[i].id >= id) id = pt[i].id + 1;
    for (int i = 0; i < NPTIMER; i++)
        if (!pt[i].used) {
            memset(&pt[i], 0, sizeof pt[i]);
            pt[i].tgid = t->tgid; pt[i].id = id; pt[i].signo = sev.signo; pt[i].notify = sev.notify;
            pt[i].tid = sev.notify == 4 ? pid_global(t, sev.tid, 0) : 0;
            pt[i].value = usev ? sev.value : (uint64_t)id;
            int k = id;
            if (copyout(t->mm, uid, &k, 4) < 0) return -EFAULT;
            pt[i].used = 1;
            return 0;
        }
    return -EAGAIN;
}

static struct ptimer *getpt(struct task *t, long id)
{
    for (int i = 0; i < NPTIMER; i++) if (pt[i].used && pt[i].tgid == t->tgid && pt[i].id == id) return &pt[i];
    return 0;
}

long sys_timer_settime(struct task *t, long id, long flags, uint64_t unew, uint64_t uold)
{
    struct ptimer *p = getpt(t, id);
    if (!p) return -EINVAL;
    int64_t v[4];
    if (copyin(t->mm, v, unew, 32) < 0) return -EFAULT;
    if (uold) itimer_out(t, uold, p->next, p->interval);
    uint64_t val = ts_in(v + 2);
    p->interval = ts_in(v);
    p->overrun = 0;
    if (!val) { p->next = 0; return 0; }
    if (flags & 1) { uint64_t e = epoch_ns(); p->next = val > e ? val - e : 1; }   /* TIMER_ABSTIME (realtime) */
    else p->next = mono_ns() + val;
    return 0;
}

long sys_timer_gettime(struct task *t, long id, uint64_t ucur)
{
    struct ptimer *p = getpt(t, id);
    if (!p) return -EINVAL;
    itimer_out(t, ucur, p->next, p->interval);
    return 0;
}

long sys_timer_getoverrun(struct task *t, long id) { struct ptimer *p = getpt(t, id); return p ? p->overrun : -EINVAL; }
long sys_timer_delete(struct task *t, long id) { struct ptimer *p = getpt(t, id); if (!p) return -EINVAL; p->used = 0; return 0; }

void ptimer_exit(int tgid) { for (int i = 0; i < NPTIMER; i++) if (pt[i].used && pt[i].tgid == tgid) pt[i].used = 0; }

/* from the timer interrupt */
void ptimer_tick(void)
{
    uint64_t now = 0;
    for (int i = 0; i < NPTIMER; i++) {
        struct ptimer *p = &pt[i];
        if (!p->used || !p->next) continue;
        if (!now) now = mono_ns();
        if (now < p->next) continue;
        if (p->interval) { uint64_t n = (now - p->next) / p->interval; p->overrun = (int)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n); p->next += (n + 1) * p->interval; }
        else p->next = 0;
        if (p->notify == 1) continue;
        struct task *x = task_by_pid(p->notify == 4 ? p->tid : p->tgid);
        if (!x || !x->used || x->state == 2) { p->used = 0; continue; }
        x->pending |= 1ULL << (p->signo - 1);
        x->si_code[p->signo - 1] = -2;                         /* SI_TIMER: for sigwaitinfo and SA_SIGINFO */
        x->si_value[p->signo - 1] = p->value;
        x->si_timer[p->signo - 1] = p->id;
    }
}
